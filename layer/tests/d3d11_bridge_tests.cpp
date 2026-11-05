// Frame Generation's Direct3D 11 bridge (D3D11Bridge) against a stand-in
// runtime whose swapchain images are Direct3D 12 textures on the bridge's
// device. WARP shares textures and fences between its Direct3D 11 and 12
// devices, so this runs without a GPU; `hw` runs it on the first hardware
// adapter (FG_TEST_ADAPTER: the one it names, test_adapter.h). The Direct3D 12
// debug layer checks every test's GPU work (and the Direct3D 11 one when
// installed). Exit code 77: this machine shares nothing between the APIs, or
// FG_TEST_ADAPTER names no adapter.
#include <windows.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <openxr/openxr_platform.h>

#include "depthxr/framegen_d3d11_bridge.h"
#include "depthxr/framegen_gpu.h"
#include "depthxr/framegen_plan.h"
#include "depthxr/session_graphics.h"
#include "test_adapter.h"
#include "test_gates.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <map>
#include <string>
#include <thread>
#include <vector>

using depthxr::D3D11Bridge;
using Microsoft::WRL::ComPtr;

namespace {

constexpr int kSkip = 77;

void Check(bool value, const std::string& message) {
    if (!value) {
        std::cerr << "FAILED: " << message << std::endl;
        test_gates::OpenAllBeforeExit();
        std::exit(1);
    }
}

bool Contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

// A figure from the bridge's summary ("name=value").
std::uint64_t SummaryValue(const std::string& summary, const std::string& name) {
    const std::size_t at = summary.find(name + "=");
    Check(at != std::string::npos, "the summary has " + name + ": " + summary);
    return std::stoull(summary.substr(at + name.size() + 1));
}

double SecondsSince(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

// The stand-in runtime.
LUID g_runtime_luid{};
bool g_requirements_offered = true;
std::vector<ComPtr<ID3D12Resource>> g_runtime_images;
std::vector<std::string> g_log; // what the bridge reported

XrResult XRAPI_CALL FakeGetRequirements(XrInstance, XrSystemId, XrGraphicsRequirementsD3D12KHR* requirements) {
    requirements->adapterLuid = g_runtime_luid;
    requirements->minFeatureLevel = D3D_FEATURE_LEVEL_11_0;
    return XR_SUCCESS;
}

XrResult XRAPI_CALL FakeGetInstanceProcAddr(XrInstance, const char* name, PFN_xrVoidFunction* function) {
    if (g_requirements_offered && std::strcmp(name, "xrGetD3D12GraphicsRequirementsKHR") == 0) {
        *function = reinterpret_cast<PFN_xrVoidFunction>(&FakeGetRequirements);
        return XR_SUCCESS;
    }
    *function = nullptr;
    return XR_ERROR_FUNCTION_UNSUPPORTED;
}

XrResult XRAPI_CALL FakeEnumerate(XrSwapchain, uint32_t capacity, uint32_t* count, XrSwapchainImageBaseHeader* images) {
    *count = static_cast<uint32_t>(g_runtime_images.size());
    if (capacity == 0) return XR_SUCCESS;
    if (capacity < g_runtime_images.size()) return XR_ERROR_SIZE_INSUFFICIENT;
    auto* d3d12_images = reinterpret_cast<XrSwapchainImageD3D12KHR*>(images);
    for (std::size_t i = 0; i < g_runtime_images.size(); ++i) {
        Check(d3d12_images[i].type == XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR, "the bridge enumerates Direct3D 12 images");
        d3d12_images[i].texture = g_runtime_images[i].Get();
    }
    return XR_SUCCESS;
}

struct Game {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11DeviceContext4> context4;
    LUID luid{};
    bool debug{false};
};

bool MakeGame(bool hardware, Game& game) {
    ComPtr<IDXGIAdapter1> adapter;
    if (hardware) {
        ComPtr<IDXGIFactory1> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
            return false;
        }
        if (const std::optional<std::string> wanted = test_adapter::Wanted()) {
            std::string why;
            adapter = test_adapter::FindDxgiAdapter(factory.Get(), *wanted, why);
            if (!adapter) {
                std::cout << why << '\n';
                return false;
            }
        } else if (FAILED(factory->EnumAdapters1(0, &adapter))) {
            return false;
        }
    }
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    const D3D_DRIVER_TYPE type = hardware ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_WARP;
    // The Direct3D 11 debug layer when its SDK layers are installed.
    game.debug = SUCCEEDED(D3D11CreateDevice(adapter.Get(), type, nullptr,
                                             D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_DEBUG, levels, 2,
                                             D3D11_SDK_VERSION, &game.device, nullptr, &game.context));
    if (!game.debug && FAILED(D3D11CreateDevice(adapter.Get(), type, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                                levels, 2, D3D11_SDK_VERSION, &game.device, nullptr, &game.context))) {
        return false;
    }
    ComPtr<IDXGIDevice> dxgi_device;
    ComPtr<IDXGIAdapter> game_adapter;
    DXGI_ADAPTER_DESC desc{};
    if (FAILED(game.device.As(&dxgi_device)) || FAILED(dxgi_device->GetAdapter(&game_adapter)) ||
        FAILED(game_adapter->GetDesc(&desc)) || FAILED(game.context.As(&game.context4))) {
        return false;
    }
    game.luid = desc.AdapterLuid;
    return true;
}

// Every error and corruption message both debug layers stored since the last check.
std::string ValidationErrors(ID3D12Device* device, Game& game) {
    std::string errors;
    ComPtr<ID3D12InfoQueue> queue12;
    if (device && SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&queue12)))) {
        for (UINT64 i = 0; i < queue12->GetNumStoredMessages(); ++i) {
            SIZE_T size = 0;
            queue12->GetMessage(i, nullptr, &size);
            std::vector<char> storage(size);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            if (SUCCEEDED(queue12->GetMessage(i, message, &size)) &&
                message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
                errors += std::string("[D3D12 ") + std::to_string(message->ID) + "] " + message->pDescription + "\n";
            }
        }
        queue12->ClearStoredMessages();
    }
    ComPtr<ID3D11InfoQueue> queue11;
    if (game.debug && SUCCEEDED(game.device.As(&queue11))) {
        for (UINT64 i = 0; i < queue11->GetNumStoredMessages(); ++i) {
            SIZE_T size = 0;
            queue11->GetMessage(i, nullptr, &size);
            std::vector<char> storage(size);
            auto* message = reinterpret_cast<D3D11_MESSAGE*>(storage.data());
            if (SUCCEEDED(queue11->GetMessage(i, message, &size)) &&
                message->Severity <= D3D11_MESSAGE_SEVERITY_ERROR) {
                errors += std::string("[D3D11 ") + std::to_string(message->ID) + "] " + message->pDescription + "\n";
            }
        }
        queue11->ClearStoredMessages();
    }
    return errors;
}

ID3D12Device* BridgeDevice(const D3D11Bridge& bridge) {
    return static_cast<const XrGraphicsBindingD3D12KHR*>(bridge.Binding())->device;
}

ID3D12CommandQueue* BridgeQueue(const D3D11Bridge& bridge) {
    return static_cast<const XrGraphicsBindingD3D12KHR*>(bridge.Binding())->queue;
}

// The stand-in runtime's swapchain: images in the render-target state, as a
// runtime hands colour images out.
void MakeRuntimeImages(ID3D12Device* device, std::uint32_t count, std::uint32_t width, std::uint32_t height,
                       DXGI_FORMAT format, std::uint32_t array_size = 1, bool render_target = true) {
    g_runtime_images.clear();
    for (std::uint32_t i = 0; i < count; ++i) {
        D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = static_cast<UINT16>(array_size);
        desc.MipLevels = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Flags = render_target ? D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET : D3D12_RESOURCE_FLAG_NONE;
        ComPtr<ID3D12Resource> image;
        Check(SUCCEEDED(device->CreateCommittedResource(
                  &heap, D3D12_HEAP_FLAG_NONE, &desc,
                  render_target ? D3D12_RESOURCE_STATE_RENDER_TARGET : D3D12_RESOURCE_STATE_COMMON, nullptr,
                  IID_PPV_ARGS(&image))),
              "the stand-in runtime makes its images");
        g_runtime_images.push_back(image);
    }
}

// Reads the centre texel of the runtime's image `index` (array slice `slice`)
// on a queue of the stand-in runtime's own, as RGBA8.
std::array<std::uint8_t, 4> ReadRuntimeImage(ID3D12Device* device, std::uint32_t index, std::uint32_t slice = 0) {
    ID3D12Resource* image = g_runtime_images[index].Get();
    const D3D12_RESOURCE_DESC desc = image->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 bytes = 0;
    const UINT subresource = slice * desc.MipLevels;
    device->GetCopyableFootprints(&desc, subresource, 1, 0, &footprint, nullptr, nullptr, &bytes);
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC buffer_desc{};
    buffer_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer_desc.Width = bytes;
    buffer_desc.Height = 1;
    buffer_desc.DepthOrArraySize = 1;
    buffer_desc.MipLevels = 1;
    buffer_desc.SampleDesc.Count = 1;
    buffer_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> buffer;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
    Check(SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer_desc,
                                                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&buffer))) &&
              SUCCEEDED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue))) &&
              SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) &&
              SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                                  IID_PPV_ARGS(&list))) &&
              SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))),
          "readback objects");
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = image;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    list->ResourceBarrier(1, &barrier);
    D3D12_TEXTURE_COPY_LOCATION source{image, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    source.SubresourceIndex = subresource;
    D3D12_TEXTURE_COPY_LOCATION destination{buffer.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    destination.PlacedFootprint = footprint;
    list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    list->ResourceBarrier(1, &barrier);
    list->Close();
    ID3D12CommandList* lists[] = {list.Get()};
    queue->ExecuteCommandLists(1, lists);
    queue->Signal(fence.Get(), 1);
    const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    fence->SetEventOnCompletion(1, event);
    Check(WaitForSingleObject(event, 10000) == WAIT_OBJECT_0, "the readback finishes");
    CloseHandle(event);
    void* mapped = nullptr;
    Check(SUCCEEDED(buffer->Map(0, nullptr, &mapped)), "map the readback");
    const auto* texel = static_cast<const std::uint8_t*>(mapped) + footprint.Offset +
                        static_cast<std::size_t>(desc.Height / 2) * footprint.Footprint.RowPitch +
                        static_cast<std::size_t>(desc.Width / 2) * 4;
    std::array<std::uint8_t, 4> result{texel[0], texel[1], texel[2], texel[3]};
    buffer->Unmap(0, nullptr);
    return result;
}

// UNORM 0.5 rounds to 127 on some drivers and 128 on others.
bool Near(const std::array<std::uint8_t, 4>& texel, const std::array<std::uint8_t, 4>& expected) {
    for (int i = 0; i < 4; ++i) {
        if (std::abs(static_cast<int>(texel[i]) - static_cast<int>(expected[i])) > 1) return false;
    }
    return true;
}

std::string Text(const std::array<std::uint8_t, 4>& texel) {
    return std::to_string(texel[0]) + "," + std::to_string(texel[1]) + "," + std::to_string(texel[2]) + "," +
           std::to_string(texel[3]);
}

// The game's rendering: a clear of `texture` (slice `slice`) through a UNORM
// render-target view, as DCS renders its sRGB images.
void Clear(Game& game, ID3D11Texture2D* texture, const std::array<std::uint8_t, 4>& color, std::uint32_t slice = 0) {
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    D3D11_RENDER_TARGET_VIEW_DESC view{};
    view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    if (desc.SampleDesc.Count > 1) {
        view.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY;
        view.Texture2DMSArray.FirstArraySlice = slice;
        view.Texture2DMSArray.ArraySize = 1;
    } else {
        view.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
        view.Texture2DArray.FirstArraySlice = slice;
        view.Texture2DArray.ArraySize = 1;
    }
    ComPtr<ID3D11RenderTargetView> target;
    Check(SUCCEEDED(game.device->CreateRenderTargetView(texture, &view, &target)), "a render-target view of a twin");
    const float rgba[4] = {color[0] / 255.0f, color[1] / 255.0f, color[2] / 255.0f, color[3] / 255.0f};
    game.context->ClearRenderTargetView(target.Get(), rgba);
}

// Direct3D 11 still works: an event query after a clear completes.
bool GameRuns(Game& game) {
    D3D11_QUERY_DESC desc{D3D11_QUERY_EVENT};
    ComPtr<ID3D11Query> query;
    if (FAILED(game.device->CreateQuery(&desc, &query))) return false;
    game.context->End(query.Get());
    game.context->Flush();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    BOOL done = FALSE;
    while (std::chrono::steady_clock::now() < deadline) {
        if (game.context->GetData(query.Get(), &done, sizeof(done), 0) == S_OK && done) return true;
        Sleep(1);
    }
    return false;
}

// A fence the test opens: Direct3D 11 (or a Direct3D 12 queue) held back until Open.
bool GpuNegativeControlsAllowed() {
    const char* no_stall = std::getenv("FG_TEST_NO_STALL");
    return !(no_stall && std::string(no_stall) == "1");
}

struct Gate {
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D11Fence> fence11;
    Gate(ID3D12Device* device, Game& game) {
        HANDLE handle = nullptr;
        Check(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence))) &&
                  SUCCEEDED(device->CreateSharedHandle(fence.Get(), nullptr, GENERIC_ALL, nullptr, &handle)),
              "a gate fence");
        ComPtr<ID3D11Device5> device5;
        Check(SUCCEEDED(game.device.As(&device5)) && SUCCEEDED(device5->OpenSharedFence(handle, IID_PPV_ARGS(&fence11))),
              "the gate opened on the game's device");
        CloseHandle(handle);
        test_gates::Add(fence.Get());
    }
    ~Gate() {
        test_gates::Remove(fence.Get());
        Open();
    }
    Gate(const Gate&) = delete;
    Gate& operator=(const Gate&) = delete;
    void HoldGame(Game& game) {
        Check(GpuNegativeControlsAllowed(), "FG_TEST_NO_STALL=1 forbids an artificial game GPU hold");
        game.context4->Wait(fence11.Get(), 1);
    }
    void HoldQueue(ID3D12CommandQueue* queue) {
        Check(GpuNegativeControlsAllowed(), "FG_TEST_NO_STALL=1 forbids an artificial GPU queue hold");
        queue->Wait(fence.Get(), 1);
    }
    void Open() { fence->Signal(1); }
};

struct Session {
    XrBaseInStructure application_struct{static_cast<XrStructureType>(1000999000)}; // after the binding
    XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    XrSessionCreateInfo info{XR_TYPE_SESSION_CREATE_INFO};
    explicit Session(ID3D11Device* device) {
        binding.next = &application_struct;
        binding.device = device;
        info.next = &binding;
        info.systemId = 1;
    }
};

XrSwapchainCreateInfo ColorInfo(std::uint32_t width, std::uint32_t height, std::uint32_t samples = 1,
                                std::uint32_t array_size = 1) {
    XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    info.format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    info.sampleCount = samples;
    info.width = width;
    info.height = height;
    info.faceCount = 1;
    info.arraySize = array_size;
    info.mipCount = 1;
    return info;
}

XrSwapchain Handle(std::uintptr_t value) {
    return reinterpret_cast<XrSwapchain>(value);
}

void BeginBridge(Game& game, D3D11Bridge& bridge, Session& session) {
    std::string reason;
    std::string description;
    g_runtime_luid = game.luid;
    g_log.clear();
    bridge.SetLog([](const std::string& message) { g_log.push_back(message); });
    Check(bridge.Begin(XR_NULL_HANDLE, &session.info, FakeGetInstanceProcAddr, reason, description) != nullptr,
          "bridged: " + reason);
    bridge.SessionCreated();
}

// The test's GPU work passed both debug layers, and the bridge reported nothing.
void CheckClean(const D3D11Bridge& bridge, Game& game, const std::string& test) {
    const std::string errors = ValidationErrors(BridgeDevice(bridge), game);
    Check(errors.empty(), test + ": the debug layers reported\n" + errors);
    std::string log;
    for (const std::string& message : g_log) log += message + "\n";
    Check(g_log.empty(), test + ": the bridge reported\n" + log);
}

void TestBegin(Game& game) {
    D3D11Bridge bridge;
    std::string reason;
    std::string description;
    Session session(game.device.Get());

    // Not a Direct3D 11 binding first.
    XrSessionCreateInfo other{XR_TYPE_SESSION_CREATE_INFO};
    other.next = &session.application_struct;
    Check(!bridge.Begin(XR_NULL_HANDLE, &other, FakeGetInstanceProcAddr, reason, description) &&
              Contains(reason, "first structure") && !bridge.QueriedD3D12Requirements(),
          "only a Direct3D 11 binding in first place is bridged: " + reason);

    // The runtime offers no Direct3D 12 requirements (XR_KHR_D3D12_enable not enabled).
    g_requirements_offered = false;
    Check(!bridge.Begin(XR_NULL_HANDLE, &session.info, FakeGetInstanceProcAddr, reason, description) &&
              Contains(reason, "xrGetD3D12GraphicsRequirementsKHR") && !bridge.QueriedD3D12Requirements(),
          "no bridge without the runtime's requirements: " + reason);
    g_requirements_offered = true;

    // The runtime composes on another adapter (its requirements were asked for).
    g_runtime_luid = game.luid;
    g_runtime_luid.LowPart ^= 0x5a5a;
    Check(!bridge.Begin(XR_NULL_HANDLE, &session.info, FakeGetInstanceProcAddr, reason, description) &&
              Contains(reason, "adapter") && bridge.QueriedD3D12Requirements(),
          "no bridge across adapters: " + reason);
    g_runtime_luid = game.luid;

    // The kill switch.
    _putenv_s("VECTORXR_D3D11_BRIDGE", "off");
    Check(!bridge.Begin(XR_NULL_HANDLE, &session.info, FakeGetInstanceProcAddr, reason, description) &&
              Contains(reason, "VECTORXR_D3D11_BRIDGE"),
          "VECTORXR_D3D11_BRIDGE=off keeps Direct3D 11: " + reason);
    _putenv_s("VECTORXR_D3D11_BRIDGE", "");

    const XrSessionCreateInfo* bridged =
        bridge.Begin(XR_NULL_HANDLE, &session.info, FakeGetInstanceProcAddr, reason, description);
    if (!bridged && Contains(reason, "tier")) {
        std::cout << "skipped: " << reason << '\n';
        std::exit(kSkip);
    }
    Check(bridged != nullptr, "the session is bridged: " + reason);
    Check(bridge.Active(), "active after Begin");
    Check(bridged->systemId == 1 && bridged->createFlags == session.info.createFlags, "the create info is kept");
    const auto* binding = static_cast<const XrGraphicsBindingD3D12KHR*>(bridged->next);
    Check(binding->type == XR_TYPE_GRAPHICS_BINDING_D3D12_KHR && binding->device && binding->queue,
          "the runtime gets a Direct3D 12 binding");
    const D3D12_COMMAND_QUEUE_DESC queue = binding->queue->GetDesc();
    Check(queue.Type == D3D12_COMMAND_LIST_TYPE_DIRECT && queue.Priority == D3D12_COMMAND_QUEUE_PRIORITY_HIGH,
          "the bridge's queue is a high-priority direct queue");
    Check(binding->next == &session.application_struct, "the application's other structures follow the binding");
    Check(bridge.Binding() == binding, "Binding() is the session's binding");
    Check(Contains(description, "sharedResourceCompatibilityTier") && Contains(description, "videoMemory="),
          "the description names the sharing tier and video memory: " + description);
    bridge.SessionCreated();
    Check(binding->next == nullptr, "after xrCreateSession the application's structures are not followed");
    Check(bridge.Drain(), "an idle bridge drains at once");
    std::string lost;
    Check(!bridge.Lost(lost), "the device is not lost");
    bridge.End();
    Check(!bridge.Active() && bridge.Binding() == nullptr, "inactive after End");
    Check(GameRuns(game), "the game's device works after End");
}

// The session log's graphics lines (QuerySessionGraphics): the game's
// Direct3D 11 device and the bridge's Direct3D 12 one both name the game's
// adapter as DXGI describes it (WARP: Microsoft's vendor ID, 0x1414).
void TestSessionGraphics(Game& game, bool hardware) {
    D3D11Bridge bridge;
    Session session(game.device.Get());
    BeginBridge(game, bridge, session);
    ComPtr<IDXGIDevice> dxgi_device;
    ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC desc{};
    Check(SUCCEEDED(game.device.As(&dxgi_device)) && SUCCEEDED(dxgi_device->GetAdapter(&adapter)) &&
              SUCCEEDED(adapter->GetDesc(&desc)),
          "the game's adapter");
    Check(hardware || desc.VendorId == 0x1414, "WARP's vendor is Microsoft's");
    const auto check_entry = [&](const depthxr::SessionGraphics& entry, const std::string& api) {
        const std::string line = depthxr::FormatSessionGraphics(entry, "stand-in", "1.0.0");
        Check(entry.api == api && entry.problem.empty(), "the " + api + " entry: " + line);
        Check(!entry.adapter.empty() && entry.vendor_id == desc.VendorId &&
                  entry.dedicated_video_memory == desc.DedicatedVideoMemory,
              "the " + api + " entry names the game's adapter: " + line);
        Check(entry.driver_version.has_value() && entry.budget.has_value() && entry.usage.has_value(),
              "the " + api + " entry has the driver version and the budget: " + line);
        Check(!entry.vulkan_driver_version, "no Vulkan driver version without Vulkan: " + line);
    };

    const std::vector<depthxr::SessionGraphics> bridged =
        depthxr::QuerySessionGraphics(session.info.next, bridge.Binding(), "Direct3D 12 (the bridge)");
    Check(bridged.size() == 2, "the game's API and the bridge's: " + std::to_string(bridged.size()));
    check_entry(bridged[0], "Direct3D 11");
    check_entry(bridged[1], "Direct3D 12 (the bridge)");
    Check(bridged[0].adapter == bridged[1].adapter && bridged[0].driver_version == bridged[1].driver_version,
          "both name the same adapter and driver");

    // A Direct3D 12 game (the bridge's device as its binding), after other structures.
    XrBaseInStructure other{static_cast<XrStructureType>(1000999000)};
    other.next = static_cast<const XrBaseInStructure*>(bridge.Binding());
    const std::vector<depthxr::SessionGraphics> native = depthxr::QuerySessionGraphics(&other, nullptr, {});
    Check(native.size() == 1, "one entry without a bridge");
    check_entry(native[0], "Direct3D 12");

    // A binding without a device, and no binding: an entry that says why.
    XrGraphicsBindingD3D11KHR no_device{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    const std::vector<depthxr::SessionGraphics> missing = depthxr::QuerySessionGraphics(&no_device, nullptr, {});
    Check(missing.size() == 1 && missing[0].api == "Direct3D 11" && !missing[0].problem.empty() &&
              !missing[0].vendor_id,
          "a binding without a device says so");
    const std::vector<depthxr::SessionGraphics> none = depthxr::QuerySessionGraphics(nullptr, nullptr, {});
    Check(none.size() == 1 && none[0].api == "none" && !none[0].problem.empty(), "no binding says so");
    CheckClean(bridge, game, "session graphics");
    bridge.End();
}

void TestCreateInfo() {
    XrSwapchainCreateInfo info = ColorInfo(64, 64, 4);
    info.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    XrSwapchainCreateInfo runtime = D3D11Bridge::RuntimeCreateInfo(info);
    Check((runtime.usageFlags & XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT) != 0 &&
              (runtime.usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT) != 0,
          "copies need the runtime's colour images as render targets and copy destinations");
    Check(runtime.sampleCount == 1, "multisampled images reach the runtime resolved");
    XrSwapchainCreateInfo depth{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    depth.usageFlags = XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    depth.format = DXGI_FORMAT_D32_FLOAT;
    depth.sampleCount = 1;
    runtime = D3D11Bridge::RuntimeCreateInfo(depth);
    Check(runtime.usageFlags == depth.usageFlags && runtime.sampleCount == 1,
          "depth swapchains reach the runtime as asked");
    // UEVR's: sampled and mutable as well.
    depth.usageFlags |= XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT;
    depth.format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    runtime = D3D11Bridge::RuntimeCreateInfo(depth);
    Check(runtime.usageFlags == depth.usageFlags, "UEVR's depth usage reaches the runtime as asked");
    // A depth format asked for without depth-stencil use: the bridge's draw
    // needs the runtime's images to be depth targets.
    depth.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    depth.format = DXGI_FORMAT_D16_UNORM;
    runtime = D3D11Bridge::RuntimeCreateInfo(depth);
    Check(runtime.usageFlags == (XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT),
          "depth images are asked for as depth targets");
}

// Frames through a three-image swapchain: each release reaches its runtime
// image, with no wait between frames (copies of an image may still be queued
// when it is released again).
void TestTwinFrames(Game& game) {
    D3D11Bridge bridge;
    Session session(game.device.Get());
    BeginBridge(game, bridge, session);
    ID3D12Device* device = BridgeDevice(bridge);
    MakeRuntimeImages(device, 3, 64, 48, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    const XrSwapchain swapchain = Handle(0x10);
    std::string reason;
    Check(bridge.AddSwapchain(swapchain, ColorInfo(64, 48), FakeEnumerate, reason), "bridged swapchain: " + reason);
    Check(bridge.Bridged(swapchain) && !bridge.Unmirrored(swapchain) && !bridge.Composed(swapchain) &&
              !bridge.HasUnmirrored(),
          "a colour swapchain is mirrored");
    const std::vector<ID3D11Texture2D*> images = bridge.Images(swapchain);
    Check(images.size() == 3, "one twin per runtime image");
    for (ID3D11Texture2D* image : images) {
        D3D11_TEXTURE2D_DESC desc{};
        image->GetDesc(&desc);
        Check(desc.Format == DXGI_FORMAT_R8G8B8A8_TYPELESS && desc.Width == 64 && desc.Height == 48 &&
                  (desc.BindFlags & D3D11_BIND_RENDER_TARGET) && (desc.BindFlags & D3D11_BIND_SHADER_RESOURCE),
              "twins are typeless render targets the game can sample");
    }
    std::array<std::array<std::uint8_t, 4>, 3> last{};
    for (std::uint32_t frame = 0; frame < 9; ++frame) {
        const std::uint32_t index = frame % 3;
        const std::array<std::uint8_t, 4> color{static_cast<std::uint8_t>(20 + 25 * frame),
                                                static_cast<std::uint8_t>(200 - 20 * frame), 128, 255};
        bridge.Acquired(swapchain, index);
        Clear(game, images[index], color);
        bridge.Released(swapchain);
        last[index] = color;
    }
    Check(bridge.WaitIdle(10000), "the bridge's queue finishes");
    for (std::uint32_t index = 0; index < 3; ++index) {
        const auto texel = ReadRuntimeImage(device, index);
        Check(Near(texel, last[index]), "runtime image " + std::to_string(index) + " holds its last release: " +
                                            Text(texel) + ", expected " + Text(last[index]));
    }
    Check(Contains(bridge.Describe(swapchain), "copies=9") && Contains(bridge.Describe(swapchain), "skippedCopies=0"),
          "nine copies: " + bridge.Describe(swapchain));
    CheckClean(bridge, game, "twin frames");
    Check(bridge.RemoveSwapchain(swapchain), "the copies into a removed swapchain finished");
    Check(!bridge.Bridged(swapchain) && bridge.Images(swapchain).empty(), "removed");
    const std::string summary = bridge.Summary();
    Check(SummaryValue(summary, "copies") == 9 && Contains(summary, "(0 live, 0 kept)"), "summary: " + summary);
    bridge.End();
    Check(GameRuns(game), "the game's device works after End");
}

// The copy runs only after the game's rendering: with Direct3D 11 held back
// before it draws, the copy does not run; once released, the runtime's image
// gets the frame.
void TestWrittenOrdering(Game& game) {
    D3D11Bridge bridge;
    Session session(game.device.Get());
    BeginBridge(game, bridge, session);
    ID3D12Device* device = BridgeDevice(bridge);
    MakeRuntimeImages(device, 2, 64, 64, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    const XrSwapchain swapchain = Handle(0x18);
    std::string reason;
    Check(bridge.AddSwapchain(swapchain, ColorInfo(64, 64), FakeEnumerate, reason), "bridged: " + reason);
    Gate gate(device, game);
    const std::array<std::uint8_t, 4> color{240, 120, 10, 255};
    bridge.Acquired(swapchain, 1);
    gate.HoldGame(game);
    Clear(game, bridge.Images(swapchain)[1], color);
    bridge.Released(swapchain);
    Sleep(150);
    std::string summary = bridge.Summary();
    Check(SummaryValue(summary, "doneCompleted") < SummaryValue(summary, "doneValue"),
          "the copy waits for the game's rendering: " + summary);
    gate.Open();
    Check(bridge.WaitIdle(10000), "the bridge's queue finishes");
    const auto texel = ReadRuntimeImage(device, 1);
    Check(Near(texel, color), "the runtime's image holds the frame drawn after the gate: " + Text(texel));
    CheckClean(bridge, game, "written ordering");
    bridge.End();
}

// An image released again while its last copy is still queued gets another
// recording of the copy (D3D12 forbids executing a command list still pending).
void TestPendingRelease(Game& game) {
    D3D11Bridge bridge;
    Session session(game.device.Get());
    BeginBridge(game, bridge, session);
    ID3D12Device* device = BridgeDevice(bridge);
    MakeRuntimeImages(device, 1, 32, 32, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    const XrSwapchain swapchain = Handle(0x1c);
    std::string reason;
    Check(bridge.AddSwapchain(swapchain, ColorInfo(32, 32), FakeEnumerate, reason), "bridged: " + reason);
    ID3D11Texture2D* image = bridge.Images(swapchain)[0];
    Gate gate(device, game);
    const std::array<std::uint8_t, 4> first{200, 20, 20, 255};
    const std::array<std::uint8_t, 4> second{20, 200, 20, 255};
    bridge.Acquired(swapchain, 0);
    gate.HoldGame(game);
    Clear(game, image, first);
    bridge.Released(swapchain); // its copy waits behind the gate
    bridge.Acquired(swapchain, 0);
    Clear(game, image, second);
    bridge.Released(swapchain);
    std::string summary = bridge.Summary();
    Check(SummaryValue(summary, "copyRecordings") == 2 && SummaryValue(summary, "copies") == 2,
          "the second release used a second recording: " + summary);
    gate.Open();
    Check(bridge.WaitIdle(10000), "the bridge's queue finishes");
    Check(Near(ReadRuntimeImage(device, 0), second), "the runtime's image holds the later frame");
    CheckClean(bridge, game, "pending release");
    bridge.End();
}

// The game renders into an image again (one image, acquired again at once):
// Direct3D 11 waits for the bridge's copy of the release before, so the
// runtime's image holds the released frame, never the next one.
void TestDoneWait(Game& game) {
    D3D11Bridge bridge;
    Session session(game.device.Get());
    BeginBridge(game, bridge, session);
    ID3D12Device* device = BridgeDevice(bridge);
    // Large, so the copy is still running when the image is acquired again.
    const std::uint32_t size = 4096;
    MakeRuntimeImages(device, 1, size, size, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    const XrSwapchain swapchain = Handle(0x20);
    std::string reason;
    Check(bridge.AddSwapchain(swapchain, ColorInfo(size, size), FakeEnumerate, reason), "bridged: " + reason);
    ID3D11Texture2D* image = bridge.Images(swapchain)[0];
    const std::array<std::uint8_t, 4> released{230, 40, 20, 255};
    const std::array<std::uint8_t, 4> next{10, 60, 240, 255};
    for (int round = 0; round < 4; ++round) {
        bridge.Acquired(swapchain, 0);
        Clear(game, image, released);
        bridge.Released(swapchain);
        bridge.Acquired(swapchain, 0);
        Clear(game, image, next);
        game.context->Flush();
        Check(bridge.WaitIdle(10000), "the bridge's queue finishes");
        const auto texel = ReadRuntimeImage(device, 0);
        Check(Near(texel, released), "the runtime's image holds the released frame: " + Text(texel));
        bridge.Released(swapchain); // the next frame, released, for the following round's acquire
        Check(bridge.WaitIdle(10000), "the bridge's queue finishes");
    }
    const std::string summary = bridge.Summary();
    std::cout << "done waits (acquires with the copy still running): " << summary << '\n';
    Check(SummaryValue(summary, "doneWaits") >= 1, "at least one acquire found its copy still running");
    CheckClean(bridge, game, "done wait");
    bridge.End();
    Check(GameRuns(game), "the game's device works after End");
}

// Multisampled images are rendered privately and resolved into the twin.
void TestResolve(Game& game) {
    D3D11Bridge bridge;
    Session session(game.device.Get());
    BeginBridge(game, bridge, session);
    ID3D12Device* device = BridgeDevice(bridge);
    MakeRuntimeImages(device, 2, 32, 32, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    const XrSwapchain swapchain = Handle(0x30);
    std::string reason;
    Check(bridge.AddSwapchain(swapchain, ColorInfo(32, 32, 4), FakeEnumerate, reason), "bridged: " + reason);
    const std::vector<ID3D11Texture2D*> images = bridge.Images(swapchain);
    D3D11_TEXTURE2D_DESC desc{};
    images[0]->GetDesc(&desc);
    Check(desc.SampleDesc.Count == 4, "the game renders four samples");
    const std::array<std::uint8_t, 4> color{90, 180, 30, 255};
    bridge.Acquired(swapchain, 1);
    Clear(game, images[1], color);
    bridge.Released(swapchain);
    Check(bridge.WaitIdle(10000), "the bridge's queue finishes");
    const auto texel = ReadRuntimeImage(device, 1);
    Check(Near(texel, color), "the resolved frame reaches the runtime: " + Text(texel));
    CheckClean(bridge, game, "resolve");
    bridge.End();
}

// Array images: every slice reaches the runtime.
void TestArray(Game& game) {
    D3D11Bridge bridge;
    Session session(game.device.Get());
    BeginBridge(game, bridge, session);
    ID3D12Device* device = BridgeDevice(bridge);
    MakeRuntimeImages(device, 2, 32, 32, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 2);
    const XrSwapchain swapchain = Handle(0x40);
    std::string reason;
    Check(bridge.AddSwapchain(swapchain, ColorInfo(32, 32, 1, 2), FakeEnumerate, reason), "bridged: " + reason);
    ID3D11Texture2D* image = bridge.Images(swapchain)[0];
    const std::array<std::uint8_t, 4> left{200, 10, 10, 255};
    const std::array<std::uint8_t, 4> right{10, 10, 200, 255};
    bridge.Acquired(swapchain, 0);
    Clear(game, image, left, 0);
    Clear(game, image, right, 1);
    bridge.Released(swapchain);
    Check(bridge.WaitIdle(10000), "the bridge's queue finishes");
    Check(Near(ReadRuntimeImage(device, 0, 0), left) && Near(ReadRuntimeImage(device, 0, 1), right),
          "both slices reach the runtime");
    CheckClean(bridge, game, "array");
    bridge.End();
}

// Depth: the four depth formats a runtime offers, as the game asks for them.
struct DepthFormat {
    DXGI_FORMAT format;
    const char* name;
    DXGI_FORMAT typeless; // the family, as some runtimes' images are
};
constexpr DepthFormat kDepthFormats[] = {
    {DXGI_FORMAT_D16_UNORM, "D16", DXGI_FORMAT_R16_TYPELESS},
    {DXGI_FORMAT_D24_UNORM_S8_UINT, "D24S8", DXGI_FORMAT_R24G8_TYPELESS},
    {DXGI_FORMAT_D32_FLOAT, "D32", DXGI_FORMAT_R32_TYPELESS},
    {DXGI_FORMAT_D32_FLOAT_S8X24_UINT, "D32S8", DXGI_FORMAT_R32G8X24_TYPELESS},
};

XrSwapchainCreateInfo DepthSwapchainInfo(DXGI_FORMAT format, std::uint32_t width, std::uint32_t height,
                                         std::uint32_t array_size, std::uint32_t samples = 1) {
    XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    info.usageFlags = XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    info.format = format;
    info.sampleCount = samples;
    info.width = width;
    info.height = height;
    info.faceCount = 1;
    info.arraySize = array_size;
    info.mipCount = 1;
    return info;
}

// The stand-in runtime's depth swapchain: images in the depth-write state, as
// XR_KHR_D3D12_enable hands depth out (PimaxXR and Virtual Desktop make them
// so when asked for depth-stencil use). `depth_target` false: a runtime that
// made them no depth targets.
void MakeRuntimeDepthImages(ID3D12Device* device, std::uint32_t count, std::uint32_t width, std::uint32_t height,
                            DXGI_FORMAT format, std::uint32_t array_size, bool depth_target = true) {
    g_runtime_images.clear();
    for (std::uint32_t i = 0; i < count; ++i) {
        D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = static_cast<UINT16>(array_size);
        desc.MipLevels = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Flags = depth_target ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL : D3D12_RESOURCE_FLAG_NONE;
        ComPtr<ID3D12Resource> image;
        Check(SUCCEEDED(device->CreateCommittedResource(
                  &heap, D3D12_HEAP_FLAG_NONE, &desc,
                  depth_target ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_COMMON, nullptr,
                  IID_PPV_ARGS(&image))),
              "the stand-in runtime makes its depth images");
        g_runtime_images.push_back(image);
    }
}

// A depth texel as stored, without stencil: D16's 16 bits, D24's 24, D32's
// float bits.
std::uint32_t DepthBits(DXGI_FORMAT family, const std::uint8_t* texel) {
    std::uint32_t bits = 0;
    if (family == DXGI_FORMAT_R16_TYPELESS) {
        std::uint16_t value = 0;
        std::memcpy(&value, texel, sizeof(value));
        return value;
    }
    std::memcpy(&bits, texel, sizeof(bits));
    return family == DXGI_FORMAT_R24G8_TYPELESS ? bits & 0xFFFFFFu : bits;
}

std::uint32_t DepthTexelBytes(DXGI_FORMAT family) {
    return family == DXGI_FORMAT_R16_TYPELESS ? 2 : family == DXGI_FORMAT_R32G8X24_TYPELESS ? 8 : 4;
}

// Every depth texel of the game's private texture's slice `slice`, read on
// its own device.
std::vector<std::uint32_t> ReadGameDepth(Game& game, ID3D11Texture2D* texture, std::uint32_t slice) {
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    D3D11_TEXTURE2D_DESC staging_desc = desc;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.BindFlags = 0;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    Check(SUCCEEDED(game.device->CreateTexture2D(&staging_desc, nullptr, &staging)), "a staging copy of game depth");
    game.context->CopyResource(staging.Get(), texture);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    const UINT subresource = D3D11CalcSubresource(0, slice, 1);
    Check(SUCCEEDED(game.context->Map(staging.Get(), subresource, D3D11_MAP_READ, 0, &mapped)), "map game depth");
    std::vector<std::uint32_t> texels;
    const std::uint32_t bytes = DepthTexelBytes(desc.Format);
    for (UINT y = 0; y < desc.Height; ++y) {
        const auto* row = static_cast<const std::uint8_t*>(mapped.pData) + static_cast<std::size_t>(y) * mapped.RowPitch;
        for (UINT x = 0; x < desc.Width; ++x) {
            texels.push_back(DepthBits(desc.Format, row + static_cast<std::size_t>(x) * bytes));
        }
    }
    game.context->Unmap(staging.Get(), subresource);
    return texels;
}

// Every depth texel of the runtime's depth image `index`, slice `slice`, read
// on a queue of the stand-in runtime's own (the depth plane: D24S8 and D32S8
// are planar in Direct3D 12).
std::vector<std::uint32_t> ReadRuntimeDepth(ID3D12Device* device, std::uint32_t index, DXGI_FORMAT family,
                                            std::uint32_t slice) {
    ID3D12Resource* image = g_runtime_images[index].Get();
    const D3D12_RESOURCE_DESC desc = image->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 bytes = 0;
    const UINT subresource = slice; // plane 0, mip 0
    device->GetCopyableFootprints(&desc, subresource, 1, 0, &footprint, nullptr, nullptr, &bytes);
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC buffer_desc{};
    buffer_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer_desc.Width = bytes;
    buffer_desc.Height = 1;
    buffer_desc.DepthOrArraySize = 1;
    buffer_desc.MipLevels = 1;
    buffer_desc.SampleDesc.Count = 1;
    buffer_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> buffer;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
    Check(SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer_desc,
                                                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&buffer))) &&
              SUCCEEDED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue))) &&
              SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) &&
              SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                                  IID_PPV_ARGS(&list))) &&
              SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))),
          "depth readback objects");
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = image;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    list->ResourceBarrier(1, &barrier);
    D3D12_TEXTURE_COPY_LOCATION source{image, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    source.SubresourceIndex = subresource;
    D3D12_TEXTURE_COPY_LOCATION destination{buffer.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    destination.PlacedFootprint = footprint;
    list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    list->ResourceBarrier(1, &barrier);
    list->Close();
    ID3D12CommandList* lists[] = {list.Get()};
    queue->ExecuteCommandLists(1, lists);
    queue->Signal(fence.Get(), 1);
    const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    fence->SetEventOnCompletion(1, event);
    Check(WaitForSingleObject(event, 10000) == WAIT_OBJECT_0, "the depth readback finishes");
    CloseHandle(event);
    void* mapped = nullptr;
    Check(SUCCEEDED(buffer->Map(0, nullptr, &mapped)), "map the depth readback");
    // The depth plane's own texel size (D32S8's plane 0 is 4 bytes a texel).
    const std::uint32_t texel_bytes = family == DXGI_FORMAT_R16_TYPELESS ? 2 : 4;
    const DXGI_FORMAT plane_family = family == DXGI_FORMAT_R32G8X24_TYPELESS ? DXGI_FORMAT_R32_TYPELESS : family;
    std::vector<std::uint32_t> texels;
    for (UINT y = 0; y < desc.Height; ++y) {
        const auto* row = static_cast<const std::uint8_t*>(mapped) + footprint.Offset +
                          static_cast<std::size_t>(y) * footprint.Footprint.RowPitch;
        for (UINT x = 0; x < desc.Width; ++x) {
            texels.push_back(DepthBits(plane_family, row + static_cast<std::size_t>(x) * texel_bytes));
        }
    }
    buffer->Unmap(0, nullptr);
    return texels;
}

// The game's depth rendering: a draw over slice `slice` of a depth image
// whose pixel shader writes a hash of the texel, the slice and `frame` as
// depth (with 0 and 1 at the first texels, the range's ends), so every texel
// differs from its neighbours and from earlier frames.
class GameDepthDraw {
  public:
    explicit GameDepthDraw(Game& game) {
        const char* source = R"(
cbuffer Frame : register(b0) { uint g_slice; uint g_frame; };
float4 VSMain(uint vertex : SV_VertexID) : SV_Position {
    const float2 uv = float2((vertex << 1) & 2, vertex & 2);
    return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}
float PSMain(float4 position : SV_Position) : SV_Depth {
    const uint x = uint(position.x);
    const uint y = uint(position.y);
    if (x == 0 && y == 0) return 0.0;
    if (x == 1 && y == 0) return 1.0;
    uint h = x * 73856093u ^ y * 19349663u ^ g_slice * 83492791u ^ g_frame * 2654435761u;
    h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
    return float(h & 0xFFFFFFu) / 16777215.0;
})";
        ComPtr<ID3DBlob> vs;
        ComPtr<ID3DBlob> ps;
        ComPtr<ID3DBlob> errors;
        Check(SUCCEEDED(D3DCompile(source, std::strlen(source), "game_depth", nullptr, nullptr, "VSMain", "vs_5_0", 0, 0,
                                   &vs, &errors)) &&
                  SUCCEEDED(D3DCompile(source, std::strlen(source), "game_depth", nullptr, nullptr, "PSMain", "ps_5_0",
                                       0, 0, &ps, &errors)),
              "the game's depth shaders compile");
        Check(SUCCEEDED(game.device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &vs_)) &&
                  SUCCEEDED(game.device->CreatePixelShader(ps->GetBufferPointer(), ps->GetBufferSize(), nullptr, &ps_)),
              "the game's depth shaders");
        D3D11_DEPTH_STENCIL_DESC depth{};
        depth.DepthEnable = TRUE;
        depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        depth.DepthFunc = D3D11_COMPARISON_ALWAYS;
        D3D11_RASTERIZER_DESC raster{};
        raster.FillMode = D3D11_FILL_SOLID;
        raster.CullMode = D3D11_CULL_NONE;
        raster.DepthClipEnable = TRUE;
        D3D11_BUFFER_DESC constants{};
        constants.ByteWidth = 16;
        constants.Usage = D3D11_USAGE_DEFAULT;
        constants.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        Check(SUCCEEDED(game.device->CreateDepthStencilState(&depth, &depth_)) &&
                  SUCCEEDED(game.device->CreateRasterizerState(&raster, &raster_)) &&
                  SUCCEEDED(game.device->CreateBuffer(&constants, nullptr, &constants_)),
              "the game's depth draw state");
    }

    // Draws, leaving `texture`'s slice bound as the depth target when `keep_bound`.
    void Draw(Game& game, ID3D11Texture2D* texture, DXGI_FORMAT format, std::uint32_t slice, std::uint32_t frame,
              bool keep_bound = false) {
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        D3D11_DEPTH_STENCIL_VIEW_DESC view{};
        view.Format = format;
        view.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
        view.Texture2DArray.FirstArraySlice = slice;
        view.Texture2DArray.ArraySize = 1;
        ComPtr<ID3D11DepthStencilView> target;
        Check(SUCCEEDED(game.device->CreateDepthStencilView(texture, &view, &target)), "a depth view of game depth");
        const std::uint32_t values[4] = {slice, frame, 0, 0};
        game.context->UpdateSubresource(constants_.Get(), 0, nullptr, values, 0, 0);
        const D3D11_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(desc.Width), static_cast<float>(desc.Height),
                                      0.0f, 1.0f};
        game.context->ClearDepthStencilView(target.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 0.5f, 0x5a);
        game.context->OMSetRenderTargets(0, nullptr, target.Get());
        game.context->OMSetDepthStencilState(depth_.Get(), 0);
        game.context->RSSetState(raster_.Get());
        game.context->RSSetViewports(1, &viewport);
        game.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        game.context->IASetInputLayout(nullptr);
        game.context->VSSetShader(vs_.Get(), nullptr, 0);
        game.context->PSSetShader(ps_.Get(), nullptr, 0);
        game.context->PSSetConstantBuffers(0, 1, constants_.GetAddressOf());
        game.context->Draw(3, 0);
        if (!keep_bound) {
            game.context->OMSetRenderTargets(0, nullptr, nullptr);
        }
    }

  private:
    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> ps_;
    ComPtr<ID3D11DepthStencilState> depth_;
    ComPtr<ID3D11RasterizerState> raster_;
    ComPtr<ID3D11Buffer> constants_;
};

// Texels of `runtime` that differ from `game`: none for D16, D32 and D32S8
// (each value written back as read), and none off by more than one step of
// 24 bits for D24S8 (its value is read normalized; most arrive exact).
struct DepthCompare {
    std::size_t texels{0};
    std::size_t exact{0};
    std::size_t off_by_one{0};
    std::size_t wrong{0};
    std::string first_wrong;
};

DepthCompare CompareDepth(const std::vector<std::uint32_t>& game, const std::vector<std::uint32_t>& runtime,
                          DXGI_FORMAT family) {
    DepthCompare compare;
    compare.texels = game.size();
    if (game.size() != runtime.size()) {
        compare.wrong = game.size();
        compare.first_wrong = "sizes differ";
        return compare;
    }
    for (std::size_t i = 0; i < game.size(); ++i) {
        if (game[i] == runtime[i]) {
            ++compare.exact;
            continue;
        }
        const auto difference = static_cast<std::int64_t>(game[i]) - static_cast<std::int64_t>(runtime[i]);
        if (family == DXGI_FORMAT_R24G8_TYPELESS && (difference == 1 || difference == -1)) {
            ++compare.off_by_one;
            continue;
        }
        if (compare.first_wrong.empty()) {
            compare.first_wrong = "texel " + std::to_string(i) + ": game " + std::to_string(game[i]) + ", runtime " +
                                  std::to_string(runtime[i]);
        }
        ++compare.wrong;
    }
    return compare;
}

// Depth reaches the runtime's depth images, value for value (D24S8: within
// one 24-bit step, CompareDepth), every slice, every image its last
// release, with the runtime's images typed or typeless (PimaxXR's and
// Virtual Desktop's are the game's format, opened from Direct3D 11): drawn
// by the game, copied into the twin by Direct3D 11 and drawn into the
// runtime's image by the bridge's queue.
void TestDepthFormats(Game& game) {
    GameDepthDraw draw(game);
    for (const DepthFormat& format : kDepthFormats) {
        for (const bool typeless : {false, true}) {
            const std::string name = std::string(format.name) + (typeless ? " (typeless runtime images)" : "");
            D3D11Bridge bridge;
            Session session(game.device.Get());
            BeginBridge(game, bridge, session);
            ID3D12Device* device = BridgeDevice(bridge);
            constexpr std::uint32_t kWidth = 61; // not a multiple of the copy's 8x8 groups
            constexpr std::uint32_t kHeight = 45;
            MakeRuntimeDepthImages(device, 3, kWidth, kHeight, typeless ? format.typeless : format.format, 2);
            const XrSwapchain swapchain = Handle(0x50);
            std::string reason;
            Check(bridge.AddSwapchain(swapchain, DepthSwapchainInfo(format.format, kWidth, kHeight, 2), FakeEnumerate,
                                      reason),
                  name + ": depth is accepted: " + reason);
            Check(bridge.Bridged(swapchain) && !bridge.Unmirrored(swapchain) && !bridge.HasUnmirrored() &&
                      Contains(bridge.Describe(swapchain), "mode=depth,"),
                  name + ": depth is mirrored: " + bridge.Describe(swapchain));
            const std::vector<ID3D11Texture2D*> images = bridge.Images(swapchain);
            Check(images.size() == 3, name + ": one private depth texture per runtime image");
            for (ID3D11Texture2D* image : images) {
                D3D11_TEXTURE2D_DESC desc{};
                image->GetDesc(&desc);
                Check((desc.BindFlags & D3D11_BIND_DEPTH_STENCIL) && (desc.BindFlags & D3D11_BIND_SHADER_RESOURCE) &&
                          desc.Format == format.typeless && desc.ArraySize == 2,
                      name + ": the game gets typeless depth textures it can sample");
            }
            // Seven frames through three images: each image's last release
            // is what its runtime image holds.
            std::array<std::array<std::vector<std::uint32_t>, 2>, 3> last;
            for (std::uint32_t frame = 0; frame < 7; ++frame) {
                const std::uint32_t index = frame % 3;
                bridge.Acquired(swapchain, index);
                for (std::uint32_t slice = 0; slice < 2; ++slice) {
                    draw.Draw(game, images[index], format.format, slice, frame);
                }
                Check(bridge.Released(swapchain), name + ": release " + std::to_string(frame) + " is copied");
                for (std::uint32_t slice = 0; slice < 2; ++slice) {
                    last[index][slice] = ReadGameDepth(game, images[index], slice);
                }
            }
            Check(bridge.WaitIdle(10000), name + ": the bridge's queue finishes");
            DepthCompare total;
            for (std::uint32_t index = 0; index < 3; ++index) {
                for (std::uint32_t slice = 0; slice < 2; ++slice) {
                    const DepthCompare compare =
                        CompareDepth(last[index][slice], ReadRuntimeDepth(device, index, format.typeless, slice),
                                     format.typeless);
                    Check(compare.wrong == 0, name + ": runtime image " + std::to_string(index) + " slice " +
                                                  std::to_string(slice) + " holds its last release's depth (" +
                                                  std::to_string(compare.wrong) + " texels wrong, " +
                                                  compare.first_wrong + ")");
                    total.texels += compare.texels;
                    total.exact += compare.exact;
                    total.off_by_one += compare.off_by_one;
                }
            }
            // The game's own first texels hold the range's ends: both arrive.
            Check(last[0][0][0] == 0 && ReadRuntimeDepth(device, 0, format.typeless, 0)[0] == 0,
                  name + ": depth 0 arrives as 0");
            std::cout << name << ": " << total.exact << "/" << total.texels << " texels exact, " << total.off_by_one
                      << " one 24-bit step off\n";
            if (format.format != DXGI_FORMAT_D24_UNORM_S8_UINT) {
                Check(total.exact == total.texels, name + ": every texel exact");
            }
            const std::string summary = bridge.Summary();
            Check(SummaryValue(summary, "depthCopies") == 7 && SummaryValue(summary, "copies") == 7 &&
                      SummaryValue(summary, "skippedCopies") == 0 && SummaryValue(summary, "depthTargetUnbinds") == 0,
                  name + ": seven depth copies, each drawn: " + summary);
            CheckClean(bridge, game, "depth " + name);
            Check(bridge.RemoveSwapchain(swapchain), name + ": the draws into a removed swapchain finished");
            bridge.End();
        }
    }
    Check(GameRuns(game), "the game's device works after the depth formats");
}

// A game still holding its depth image as the depth target at release (it
// draws, then releases without unbinding), with compute state of its own
// bound: the copy reads the depth all the same, and the game's depth target
// and compute state are as it left them.
void TestDepthStillBound(Game& game) {
    GameDepthDraw draw(game);
    D3D11Bridge bridge;
    Session session(game.device.Get());
    BeginBridge(game, bridge, session);
    ID3D12Device* device = BridgeDevice(bridge);
    MakeRuntimeDepthImages(device, 2, 40, 24, DXGI_FORMAT_D32_FLOAT, 1);
    const XrSwapchain swapchain = Handle(0x54);
    std::string reason;
    Check(bridge.AddSwapchain(swapchain, DepthSwapchainInfo(DXGI_FORMAT_D32_FLOAT, 40, 24, 1), FakeEnumerate, reason),
          "bridged: " + reason);
    ID3D11Texture2D* image = bridge.Images(swapchain)[1];

    // The game's compute state: a shader, a read and a write of its own.
    const char* source = "RWTexture2D<float> o : register(u0); Texture2D<float> i : register(t0);"
                         "[numthreads(1,1,1)] void CSMain(uint3 id : SV_DispatchThreadID) { o[id.xy] = i[id.xy]; }";
    ComPtr<ID3DBlob> blob;
    Check(SUCCEEDED(D3DCompile(source, std::strlen(source), "game_cs", nullptr, nullptr, "CSMain", "cs_5_0", 0, 0,
                               &blob, nullptr)),
          "the game's compute shader compiles");
    ComPtr<ID3D11ComputeShader> game_shader;
    Check(SUCCEEDED(game.device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr,
                                                     &game_shader)),
          "the game's compute shader");
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = 8;
    desc.Height = 8;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R32_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    ComPtr<ID3D11Texture2D> game_read_texture;
    ComPtr<ID3D11Texture2D> game_write_texture;
    ComPtr<ID3D11ShaderResourceView> game_read;
    ComPtr<ID3D11UnorderedAccessView> game_write;
    Check(SUCCEEDED(game.device->CreateTexture2D(&desc, nullptr, &game_read_texture)) &&
              SUCCEEDED(game.device->CreateTexture2D(&desc, nullptr, &game_write_texture)) &&
              SUCCEEDED(game.device->CreateShaderResourceView(game_read_texture.Get(), nullptr, &game_read)) &&
              SUCCEEDED(game.device->CreateUnorderedAccessView(game_write_texture.Get(), nullptr, &game_write)),
          "the game's compute views");
    game.context->CSSetShader(game_shader.Get(), nullptr, 0);
    game.context->CSSetShaderResources(0, 1, game_read.GetAddressOf());
    game.context->CSSetUnorderedAccessViews(0, 1, game_write.GetAddressOf(), nullptr);

    bridge.Acquired(swapchain, 1);
    draw.Draw(game, image, DXGI_FORMAT_D32_FLOAT, 0, 3, /*keep_bound=*/true);
    Check(bridge.Released(swapchain), "the release is copied");

    ComPtr<ID3D11DepthStencilView> bound;
    game.context->OMGetRenderTargets(0, nullptr, &bound);
    ComPtr<ID3D11Resource> bound_resource;
    if (bound) bound->GetResource(&bound_resource);
    Check(bound_resource.Get() == image, "the game's depth target is bound again after the copy");
    ComPtr<ID3D11ComputeShader> shader;
    game.context->CSGetShader(&shader, nullptr, nullptr);
    ComPtr<ID3D11ShaderResourceView> read;
    game.context->CSGetShaderResources(0, 1, &read);
    ComPtr<ID3D11UnorderedAccessView> write;
    game.context->CSGetUnorderedAccessViews(0, 1, &write);
    Check(shader.Get() == game_shader.Get() && read.Get() == game_read.Get() && write.Get() == game_write.Get(),
          "the game's compute shader, read and write are bound as it left them");

    game.context->OMSetRenderTargets(0, nullptr, nullptr);
    const std::vector<std::uint32_t> game_depth = ReadGameDepth(game, image, 0);
    Check(bridge.WaitIdle(10000), "the bridge's queue finishes");
    const DepthCompare compare =
        CompareDepth(game_depth, ReadRuntimeDepth(device, 1, DXGI_FORMAT_R32_TYPELESS, 0), DXGI_FORMAT_R32_TYPELESS);
    Check(compare.exact == compare.texels, "depth still bound at release arrives exact (" +
                                               std::to_string(compare.wrong) + " wrong, " + compare.first_wrong + ")");
    Check(SummaryValue(bridge.Summary(), "depthTargetUnbinds") == 1, "the unbind is counted: " + bridge.Summary());
    // The copy's own time on the game's thread, apart from the handover's.
    Check(Contains(bridge.Summary(), ", depthCopyMs=") && Contains(bridge.Summary(), ", handoverMs="),
          "the depth copy's time is summarized: " + bridge.Summary());
    game.context->CSSetShader(nullptr, nullptr, 0);
    ID3D11ShaderResourceView* no_read = nullptr;
    ID3D11UnorderedAccessView* no_write = nullptr;
    game.context->CSSetShaderResources(0, 1, &no_read);
    game.context->CSSetUnorderedAccessViews(0, 1, &no_write, nullptr);
    CheckClean(bridge, game, "depth still bound");
    bridge.End();
}

// The game's output-merger and compute bindings, as Direct3D 11 reports them:
// what a depth copy must leave as it found.
struct ContextState {
    std::array<ComPtr<ID3D11RenderTargetView>, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> targets;
    ComPtr<ID3D11DepthStencilView> depth_target;
    std::array<ComPtr<ID3D11UnorderedAccessView>, D3D11_PS_CS_UAV_REGISTER_COUNT> om_writes;
    ComPtr<ID3D11ComputeShader> shader;
    std::array<ComPtr<ID3D11ShaderResourceView>, 8> cs_reads;
    std::array<ComPtr<ID3D11UnorderedAccessView>, D3D11_PS_CS_UAV_REGISTER_COUNT> cs_writes;
};

ContextState ReadContextState(Game& game) {
    ContextState state;
    ID3D11RenderTargetView* targets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    ID3D11DepthStencilView* depth_target = nullptr;
    ID3D11UnorderedAccessView* om_writes[D3D11_PS_CS_UAV_REGISTER_COUNT]{};
    game.context->OMGetRenderTargetsAndUnorderedAccessViews(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, targets,
                                                            &depth_target, 0, D3D11_PS_CS_UAV_REGISTER_COUNT,
                                                            om_writes);
    for (std::size_t i = 0; i < state.targets.size(); ++i) state.targets[i].Attach(targets[i]);
    state.depth_target.Attach(depth_target);
    for (std::size_t i = 0; i < state.om_writes.size(); ++i) state.om_writes[i].Attach(om_writes[i]);
    game.context->CSGetShader(&state.shader, nullptr, nullptr);
    ID3D11ShaderResourceView* cs_reads[8]{};
    game.context->CSGetShaderResources(0, 8, cs_reads);
    for (std::size_t i = 0; i < state.cs_reads.size(); ++i) state.cs_reads[i].Attach(cs_reads[i]);
    ID3D11UnorderedAccessView* cs_writes[D3D11_PS_CS_UAV_REGISTER_COUNT]{};
    game.context->CSGetUnorderedAccessViews(0, D3D11_PS_CS_UAV_REGISTER_COUNT, cs_writes);
    for (std::size_t i = 0; i < state.cs_writes.size(); ++i) state.cs_writes[i].Attach(cs_writes[i]);
    return state;
}

// Where `after` differs from `before`, slot by slot (empty: nowhere).
std::string StateDifferences(const ContextState& before, const ContextState& after) {
    std::string differences;
    const auto compare = [&](const char* name, std::size_t slot, const void* was, const void* is) {
        if (was != is) {
            differences += std::string(name) + "[" + std::to_string(slot) + "] " + (was ? "bound" : "empty") +
                           " -> " + (is ? (was ? "another view" : "bound") : "empty") + "; ";
        }
    };
    for (std::size_t i = 0; i < before.targets.size(); ++i)
        compare("render target", i, before.targets[i].Get(), after.targets[i].Get());
    compare("depth target", 0, before.depth_target.Get(), after.depth_target.Get());
    for (std::size_t i = 0; i < before.om_writes.size(); ++i)
        compare("output-merger UAV", i, before.om_writes[i].Get(), after.om_writes[i].Get());
    compare("compute shader", 0, before.shader.Get(), after.shader.Get());
    for (std::size_t i = 0; i < before.cs_reads.size(); ++i)
        compare("compute SRV", i, before.cs_reads[i].Get(), after.cs_reads[i].Get());
    for (std::size_t i = 0; i < before.cs_writes.size(); ++i)
        compare("compute UAV", i, before.cs_writes[i].Get(), after.cs_writes[i].Get());
    return differences;
}

// An append buffer's hidden counter, as the GPU holds it.
std::uint32_t Counter(Game& game, ID3D11UnorderedAccessView* view) {
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = 16;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Buffer> staging;
    Check(SUCCEEDED(game.device->CreateBuffer(&desc, nullptr, &staging)), "a counter readback buffer");
    game.context->CopyStructureCount(staging.Get(), 0, view);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    Check(SUCCEEDED(game.context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)), "map the counter readback");
    const std::uint32_t value = *static_cast<const std::uint32_t*>(mapped.pData);
    game.context->Unmap(staging.Get(), 0);
    return value;
}

// The game's views for the bindings below: colour targets and unordered-
// access textures of the depth's size (a texture each: views 8 to 11 are
// for compute, apart from the output merger's 0 to 7), and append buffers.
struct GameViews {
    std::vector<ComPtr<ID3D11RenderTargetView>> targets;
    std::vector<ComPtr<ID3D11UnorderedAccessView>> writes;
    std::vector<ComPtr<ID3D11UnorderedAccessView>> appends;
    std::vector<ComPtr<ID3D11ShaderResourceView>> reads;
    ComPtr<ID3D11ComputeShader> shader;
    ComPtr<ID3D11DepthStencilView> other_depth; // a depth target that is not the swapchain's

    GameViews(Game& game, std::uint32_t width, std::uint32_t height) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_R32_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
        for (int i = 0; i < 12; ++i) {
            ComPtr<ID3D11Texture2D> texture;
            ComPtr<ID3D11RenderTargetView> target;
            ComPtr<ID3D11UnorderedAccessView> write;
            ComPtr<ID3D11ShaderResourceView> read;
            Check(SUCCEEDED(game.device->CreateTexture2D(&desc, nullptr, &texture)) &&
                      SUCCEEDED(game.device->CreateRenderTargetView(texture.Get(), nullptr, &target)) &&
                      SUCCEEDED(game.device->CreateUnorderedAccessView(texture.Get(), nullptr, &write)) &&
                      SUCCEEDED(game.device->CreateShaderResourceView(texture.Get(), nullptr, &read)),
                  "the game's views");
            targets.push_back(target);
            writes.push_back(write);
            reads.push_back(read);
        }
        for (int i = 0; i < 2; ++i) {
            D3D11_BUFFER_DESC buffer{};
            buffer.ByteWidth = 64 * 4;
            buffer.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
            buffer.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
            buffer.StructureByteStride = 4;
            D3D11_UNORDERED_ACCESS_VIEW_DESC view{};
            view.Format = DXGI_FORMAT_UNKNOWN;
            view.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
            view.Buffer.NumElements = 64;
            view.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_APPEND;
            ComPtr<ID3D11Buffer> resource;
            ComPtr<ID3D11UnorderedAccessView> append;
            Check(SUCCEEDED(game.device->CreateBuffer(&buffer, nullptr, &resource)) &&
                      SUCCEEDED(game.device->CreateUnorderedAccessView(resource.Get(), &view, &append)),
                  "the game's append buffers");
            appends.push_back(append);
        }
        const char* source = "AppendStructuredBuffer<uint> o : register(u0); Texture2D<float> i : register(t0);"
                             "[numthreads(1,1,1)] void CSMain() { o.Append(asuint(i[uint2(0,0)])); }";
        ComPtr<ID3DBlob> blob;
        Check(SUCCEEDED(D3DCompile(source, std::strlen(source), "game_cs", nullptr, nullptr, "CSMain", "cs_5_0", 0, 0,
                                   &blob, nullptr)) &&
                  SUCCEEDED(game.device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr,
                                                             &shader)),
              "the game's compute shader");
        D3D11_TEXTURE2D_DESC depth = desc;
        depth.Format = DXGI_FORMAT_D32_FLOAT;
        depth.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        ComPtr<ID3D11Texture2D> depth_texture;
        Check(SUCCEEDED(game.device->CreateTexture2D(&depth, nullptr, &depth_texture)) &&
                  SUCCEEDED(game.device->CreateDepthStencilView(depth_texture.Get(), nullptr, &other_depth)),
              "the game's other depth target");
    }
};

// Unbinds everything the cases below bind.
void ClearContextState(Game& game) {
    game.context->OMSetRenderTargetsAndUnorderedAccessViews(0, nullptr, nullptr, 0, 0, nullptr, nullptr);
    game.context->CSSetShader(nullptr, nullptr, 0);
    ID3D11ShaderResourceView* no_reads[8]{};
    ID3D11UnorderedAccessView* no_writes[D3D11_PS_CS_UAV_REGISTER_COUNT]{};
    game.context->CSSetShaderResources(0, 8, no_reads);
    game.context->CSSetUnorderedAccessViews(0, D3D11_PS_CS_UAV_REGISTER_COUNT, no_writes, nullptr);
}

// The game's output merger at a release, its depth image still bound or not,
// beside render targets (sparse ones too) and pixel-shader unordered-access
// views in the slots past them (an append buffer's hidden counter among
// them), with compute state of its own (another append buffer in the copy's
// slot): after the copy every binding is in its slot as the game left it,
// both counters hold their values, and the depth arrives all the same. A
// copy that set all eight render target slots again would unbind every
// one of those views; a depth swapchain the bridge keeps private changes
// nothing either.
void TestDepthKeepsGameState(Game& game) {
    constexpr std::uint32_t kWidth = 40;
    constexpr std::uint32_t kHeight = 24;
    GameDepthDraw draw(game);
    GameViews views(game, kWidth, kHeight);
    struct Case {
        const char* name;
        std::vector<int> targets; // render target slots: a view, or -1 for none
        bool depth_bound;         // the swapchain's depth target (else `other_depth`, or none)
        bool other_depth;
        UINT write_start;
        std::vector<int> writes; // output-merger UAVs from write_start: a view, -1 none, 100 + n append n
    };
    const Case cases[] = {
        {"no render targets, UAV 1", {}, true, false, 1, {0}},
        {"no render targets, UAV 0", {}, true, false, 0, {0}},
        {"render target 0, UAV 1", {0}, true, false, 1, {1}},
        {"sparse render targets 0 and 2, UAVs 3 and 5", {0, -1, 2}, true, false, 3, {3, -1, 4}},
        {"render target 0, UAVs 1 to 4 with an append buffer", {0}, true, false, 1, {1, 100, 2, 3}},
        {"render targets 0 to 6, UAV 7", {0, 1, 2, 3, 4, 5, 6}, true, false, 7, {7}},
        {"depth already unbound, render target 0, UAV 1", {0}, false, false, 1, {1}},
        {"another depth target, UAV 1", {}, false, true, 1, {1}},
    };
    for (const bool mirrored : {true, false}) {
        if (!mirrored) _putenv_s("VECTORXR_D3D11_BRIDGE_DEPTH", "private");
        D3D11Bridge bridge;
        Session session(game.device.Get());
        BeginBridge(game, bridge, session);
        if (!mirrored) _putenv_s("VECTORXR_D3D11_BRIDGE_DEPTH", "");
        ID3D12Device* device = BridgeDevice(bridge);
        MakeRuntimeDepthImages(device, 2, kWidth, kHeight, DXGI_FORMAT_D32_FLOAT, 1);
        const XrSwapchain swapchain = Handle(0x55);
        std::string reason;
        Check(bridge.AddSwapchain(swapchain, DepthSwapchainInfo(DXGI_FORMAT_D32_FLOAT, kWidth, kHeight, 1),
                                  FakeEnumerate, reason),
              "bridged: " + reason);
        Check(bridge.Unmirrored(swapchain) == !mirrored, "the depth is mirrored or private as asked");
        std::uint64_t unbinds = 0;
        std::uint32_t frame = 0;
        for (const Case& test : cases) {
            const std::string name = std::string(mirrored ? "" : "private depth: ") + test.name;
            const std::uint32_t index = frame % 2;
            ID3D11Texture2D* image = bridge.Images(swapchain)[index];
            bridge.Acquired(swapchain, index);
            draw.Draw(game, image, DXGI_FORMAT_D32_FLOAT, 0, frame++, /*keep_bound=*/true);
            ComPtr<ID3D11DepthStencilView> depth_target;
            game.context->OMGetRenderTargets(0, nullptr, &depth_target);
            ClearContextState(game);

            std::vector<ID3D11RenderTargetView*> targets;
            for (const int target : test.targets) targets.push_back(target < 0 ? nullptr : views.targets[target].Get());
            std::vector<ID3D11UnorderedAccessView*> writes;
            std::vector<UINT> counts;
            ID3D11UnorderedAccessView* om_append = nullptr;
            for (const int write : test.writes) {
                if (write >= 100) {
                    om_append = views.appends[0].Get();
                    writes.push_back(om_append);
                    counts.push_back(5);
                } else {
                    writes.push_back(write < 0 ? nullptr : views.writes[write].Get());
                    counts.push_back(0);
                }
            }
            ID3D11DepthStencilView* bound_depth = test.depth_bound   ? depth_target.Get()
                                                  : test.other_depth ? views.other_depth.Get()
                                                                     : nullptr;
            game.context->OMSetRenderTargetsAndUnorderedAccessViews(
                static_cast<UINT>(targets.size()), targets.empty() ? nullptr : targets.data(), bound_depth,
                test.write_start, static_cast<UINT>(writes.size()), writes.data(), counts.data());
            // Compute state of the game's own: its shader, reads in slots 0
            // and 3, an append buffer in slot 0 (the copy's) and a write in 2.
            const UINT cs_count = 7;
            ID3D11UnorderedAccessView* cs_append = views.appends[1].Get();
            game.context->CSSetShader(views.shader.Get(), nullptr, 0);
            game.context->CSSetShaderResources(0, 1, views.reads[8].GetAddressOf());
            game.context->CSSetShaderResources(3, 1, views.reads[9].GetAddressOf());
            game.context->CSSetUnorderedAccessViews(0, 1, &cs_append, &cs_count);
            game.context->CSSetUnorderedAccessViews(2, 1, views.writes[10].GetAddressOf(), nullptr);

            const ContextState before = ReadContextState(game);
            // Every view went where it was asked to (the case is the state it names).
            for (std::size_t i = 0; i < targets.size(); ++i)
                Check(before.targets[i].Get() == targets[i], name + ": render target " + std::to_string(i) + " bound");
            for (std::size_t i = 0; i < writes.size(); ++i)
                Check(before.om_writes[test.write_start + i].Get() == writes[i],
                      name + ": UAV " + std::to_string(test.write_start + i) + " bound");
            Check(before.depth_target.Get() == bound_depth, name + ": depth target as asked");

            bridge.Released(swapchain);
            const ContextState after = ReadContextState(game);
            const std::string differences = StateDifferences(before, after);
            Check(differences.empty(), name + ": the game's bindings are as it left them: " + differences);
            if (om_append) {
                Check(Counter(game, om_append) == 5, name + ": the output merger's append counter keeps its value (" +
                                                         std::to_string(Counter(game, om_append)) + ")");
            }
            Check(Counter(game, cs_append) == 7,
                  name + ": the compute append counter keeps its value (" + std::to_string(Counter(game, cs_append)) + ")");
            unbinds += mirrored && test.depth_bound ? 1 : 0;
            Check(SummaryValue(bridge.Summary(), "depthTargetUnbinds") == unbinds,
                  name + ": the depth target is unbound only while it is the image: " + bridge.Summary());

            ClearContextState(game);
            if (mirrored) {
                const std::vector<std::uint32_t> game_depth = ReadGameDepth(game, image, 0);
                Check(bridge.WaitIdle(10000), name + ": the bridge's queue finishes");
                const DepthCompare compare = CompareDepth(
                    game_depth, ReadRuntimeDepth(device, index, DXGI_FORMAT_R32_TYPELESS, 0), DXGI_FORMAT_R32_TYPELESS);
                Check(compare.exact == compare.texels, name + ": the depth arrives exact (" +
                                                           std::to_string(compare.wrong) + " wrong, " +
                                                           compare.first_wrong + ")");
            }
        }
        const std::string summary = bridge.Summary();
        Check(SummaryValue(summary, "depthCopies") == (mirrored ? std::size(cases) : 0),
              std::string(mirrored ? "" : "private depth: ") + "one copy a release: " + summary);
        Check(bridge.WaitIdle(10000), "the bridge's queue finishes");
        CheckClean(bridge, game, mirrored ? "depth keeps the game's state" : "private depth keeps the game's state");
        bridge.End();
    }
    Check(GameRuns(game), "the game's device works after its state was kept");
}

// Another thread of the game's, with multithread protection on, issues its
// own compute work while a depth copy has its state bound: it runs only once
// the copy has put the game's state back (it never sees the copy's shader),
// and what it binds stays bound (the copy's restore does not overwrite it).
// Each Direct3D 11 call being serialized on its own would let it in between.
// `game_protected` false: the game never turned protection on, and the other
// thread is a render thread that never calls OpenXR (the bridge sees one
// thread only): mirroring a depth swapchain turns it on, says so once, and
// the other thread is kept out all the same. Without that, its calls ran
// inside the copy (protection stayed off).
void TestDepthCopyHoldsContext(Game& game, bool game_protected = true) {
    const std::string name = game_protected ? "depth copy holds the context"
                                            : "depth copy holds the context from a thread that never calls OpenXR";
    ComPtr<ID3D11Multithread> multithread;
    Check(SUCCEEDED(game.context.As(&multithread)), "the game's context has ID3D11Multithread");
    multithread->SetMultithreadProtected(game_protected ? TRUE : FALSE);
    GameDepthDraw draw(game);
    GameViews views(game, 32, 32);
    D3D11Bridge bridge;
    Session session(game.device.Get());
    BeginBridge(game, bridge, session);
    ID3D12Device* device = BridgeDevice(bridge);
    MakeRuntimeDepthImages(device, 2, 32, 32, DXGI_FORMAT_D32_FLOAT, 1);
    const XrSwapchain swapchain = Handle(0x56);
    std::string reason;
    Check(bridge.AddSwapchain(swapchain, DepthSwapchainInfo(DXGI_FORMAT_D32_FLOAT, 32, 32, 1), FakeEnumerate, reason),
          "bridged: " + reason);
    ID3D11Texture2D* image = bridge.Images(swapchain)[0];

    // The game's other compute shader, the other thread's.
    const char* source = "RWTexture2D<float> o : register(u0);"
                         "[numthreads(1,1,1)] void CSMain(uint3 id : SV_DispatchThreadID) { o[id.xy] = 42.0; }";
    ComPtr<ID3DBlob> blob;
    ComPtr<ID3D11ComputeShader> other_shader;
    Check(SUCCEEDED(D3DCompile(source, std::strlen(source), "other_cs", nullptr, nullptr, "CSMain", "cs_5_0", 0, 0,
                               &blob, nullptr)) &&
              SUCCEEDED(game.device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr,
                                                         &other_shader)),
          "the other thread's compute shader");
    game.context->CSSetShader(views.shader.Get(), nullptr, 0);

    const HANDLE inside = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::atomic<bool> copy_bound{false}; // the copy's state is bound (between the hook's start and its end)
    std::atomic<bool> other_ran_inside{false};
    ComPtr<ID3D11ComputeShader> other_saw;
    bridge.SetDepthCopyHookForTest([&] {
        copy_bound = true;
        SetEvent(inside);
        // Long enough for the other thread's calls to land here, were they let in.
        Sleep(200);
        copy_bound = false;
    });
    std::thread other([&] {
        WaitForSingleObject(inside, 10000);
        // With the copy's state bound, a call let in reads the copy's shader.
        game.context->CSGetShader(&other_saw, nullptr, nullptr);
        other_ran_inside = other_ran_inside || copy_bound;
        game.context->CSSetShader(other_shader.Get(), nullptr, 0);
        game.context->CSSetUnorderedAccessViews(0, 1, views.writes[0].GetAddressOf(), nullptr);
        other_ran_inside = other_ran_inside || copy_bound;
        game.context->Dispatch(1, 1, 1);
    });
    bridge.Acquired(swapchain, 0);
    draw.Draw(game, image, DXGI_FORMAT_D32_FLOAT, 0, 1);
    Check(bridge.Released(swapchain), "the release is copied");
    other.join();
    CloseHandle(inside);
    bridge.SetDepthCopyHookForTest(nullptr);

    Check(!other_ran_inside, name + ": the other thread's calls wait for the copy to put the game's state back");
    Check(other_saw.Get() == views.shader.Get(), name + ": the other thread sees the game's own compute shader");
    // Checked after the interleaving, which shows what protection being off would let in.
    Check(multithread->GetMultithreadProtected() != FALSE,
          name + ": a mirrored depth swapchain leaves multithread protection on");
    Check(Contains(bridge.Describe(swapchain),
                   game_protected ? "multithreadProtection=on" : "multithreadProtection=turned on"),
          name + ": the swapchain's description says so: " + bridge.Describe(swapchain));
    Check(g_log.empty(), name + ": nothing to report");
    Check(bridge.AddSwapchain(Handle(0x57), DepthSwapchainInfo(DXGI_FORMAT_D32_FLOAT, 32, 32, 1), FakeEnumerate,
                              reason) &&
              Contains(bridge.Describe(Handle(0x57)), "multithreadProtection=on") &&
              !Contains(bridge.Describe(Handle(0x57)), "turned on"),
          name + ": a second depth swapchain finds it on: " + bridge.Describe(Handle(0x57)));
    Check(Contains(bridge.Summary(), "secondThread=no, multithreadProtected=yes"), name + ": " + bridge.Summary());
    ComPtr<ID3D11ComputeShader> shader;
    game.context->CSGetShader(&shader, nullptr, nullptr);
    ComPtr<ID3D11UnorderedAccessView> write;
    game.context->CSGetUnorderedAccessViews(0, 1, &write);
    Check(shader.Get() == other_shader.Get() && write.Get() == views.writes[0].Get(),
          "what the other thread bound stays bound");

    ClearContextState(game);
    const std::vector<std::uint32_t> game_depth = ReadGameDepth(game, image, 0);
    Check(bridge.WaitIdle(10000), "the bridge's queue finishes");
    const DepthCompare compare =
        CompareDepth(game_depth, ReadRuntimeDepth(device, 0, DXGI_FORMAT_R32_TYPELESS, 0), DXGI_FORMAT_R32_TYPELESS);
    Check(compare.exact == compare.texels, "the depth arrives exact beside the other thread's work (" +
                                               std::to_string(compare.wrong) + " wrong, " + compare.first_wrong + ")");
    g_log.clear();
    CheckClean(bridge, game, name);
    bridge.End();
    multithread->SetMultithreadProtected(FALSE);
}

// A depth image released again before the bridge's queue drew its last
// release: Direct3D 11 waits for that draw before writing the twin again, so
// the runtime gets each release in turn and the last one at the end.
void TestDepthDoneWait(Game& game) {
    GameDepthDraw draw(game);
    D3D11Bridge bridge;
    Session session(game.device.Get());
    BeginBridge(game, bridge, session);
    ID3D12Device* device = BridgeDevice(bridge);
    const std::uint32_t width = 2048; // large, so a draw is still running at the next release
    const std::uint32_t height = 2048;
    MakeRuntimeDepthImages(device, 1, width, height, DXGI_FORMAT_D16_UNORM, 1);
    const XrSwapchain swapchain = Handle(0x58);
    std::string reason;
    Check(bridge.AddSwapchain(swapchain, DepthSwapchainInfo(DXGI_FORMAT_D16_UNORM, width, height, 1), FakeEnumerate,
                              reason),
          "bridged: " + reason);
    ID3D11Texture2D* image = bridge.Images(swapchain)[0];
    for (std::uint32_t frame = 0; frame < 6; ++frame) {
        bridge.Acquired(swapchain, 0);
        draw.Draw(game, image, DXGI_FORMAT_D16_UNORM, 0, frame);
        Check(bridge.Released(swapchain), "release " + std::to_string(frame) + " is copied");
    }
    const std::vector<std::uint32_t> game_depth = ReadGameDepth(game, image, 0);
    Check(bridge.WaitIdle(10000), "the bridge's queue finishes");
    const DepthCompare compare =
        CompareDepth(game_depth, ReadRuntimeDepth(device, 0, DXGI_FORMAT_R16_TYPELESS, 0), DXGI_FORMAT_R16_TYPELESS);
    Check(compare.exact == compare.texels, "the last release's depth arrives exact (" + std::to_string(compare.wrong) +
                                               " wrong, " + compare.first_wrong + ")");
    std::cout << "depth done waits (releases whose twin was still being drawn): " << bridge.Summary() << '\n';
    // D16's twin is R16_UNORM: 2048x2048 at two bytes a texel.
    Check(SummaryValue(bridge.Summary(), "liveTwinMiB") == 8, "a D16 twin takes two bytes a texel: " + bridge.Summary());
    CheckClean(bridge, game, "depth done wait");
    bridge.End();
}

// Depth the bridge cannot mirror stays private (no copies; the caller learns
// to leave it out): multisampled, runtime images that are no depth targets
// (reported), and VECTORXR_D3D11_BRIDGE_DEPTH=private (not reported).
void TestDepthPrivate(Game& game) {
    const auto check_private = [&](const std::string& name, const XrSwapchainCreateInfo& info, bool depth_target,
                                   const std::string& why, bool reported) {
        D3D11Bridge bridge;
        Session session(game.device.Get());
        BeginBridge(game, bridge, session);
        MakeRuntimeDepthImages(BridgeDevice(bridge), 2, 32, 32, static_cast<DXGI_FORMAT>(info.format), 1, depth_target);
        const XrSwapchain swapchain = Handle(0x5c);
        std::string reason;
        Check(bridge.AddSwapchain(swapchain, info, FakeEnumerate, reason), name + ": depth is accepted: " + reason);
        Check(bridge.Bridged(swapchain) && bridge.Unmirrored(swapchain) && bridge.HasUnmirrored(),
              name + ": depth is not mirrored");
        Check(Contains(bridge.Describe(swapchain), "mode=private-depth (" + why),
              name + ": the swapchain says why: " + bridge.Describe(swapchain));
        D3D11_TEXTURE2D_DESC desc{};
        bridge.Images(swapchain)[0]->GetDesc(&desc);
        Check((desc.BindFlags & D3D11_BIND_DEPTH_STENCIL) && desc.Format == DXGI_FORMAT_R32_TYPELESS,
              name + ": the game gets a typeless depth texture");
        bridge.Acquired(swapchain, 0);
        bridge.Released(swapchain);
        Check(SummaryValue(bridge.Summary(), "copies") == 0 && SummaryValue(bridge.Summary(), "depthCopies") == 0,
              name + ": no copies of depth: " + bridge.Summary());
        Check(g_log.size() == (reported ? 1u : 0u) && (!reported || Contains(g_log[0], "stays the game's own")),
              name + ": " + (reported ? "reported once" : "not reported"));
        g_log.clear();
        Check(bridge.RemoveSwapchain(swapchain), name + ": depth removes at once");
        Check(!bridge.HasUnmirrored(), name + ": no depth once removed");
        CheckClean(bridge, game, "depth " + name);
        bridge.End();
    };
    check_private("multisampled", DepthSwapchainInfo(DXGI_FORMAT_D32_FLOAT, 32, 32, 1, 4), true, "multisampled", true);
    check_private("no depth target", DepthSwapchainInfo(DXGI_FORMAT_D32_FLOAT, 32, 32, 1), false,
                  "the runtime's depth image is not a depth target", true);
    _putenv_s("VECTORXR_D3D11_BRIDGE_DEPTH", "private");
    check_private("switched off", DepthSwapchainInfo(DXGI_FORMAT_D32_FLOAT, 32, 32, 1), true,
                  "VECTORXR_D3D11_BRIDGE_DEPTH=private", false);
    _putenv_s("VECTORXR_D3D11_BRIDGE_DEPTH", "");
}

// Frame Generation's own swapchains' images (its Track's enumerations), by swapchain.
std::map<XrSwapchain, std::vector<ID3D12Resource*>> g_framegen_images;

XrResult XRAPI_CALL FrameGenEnumerate(XrSwapchain swapchain, uint32_t capacity, uint32_t* count,
                                      XrSwapchainImageBaseHeader* images) {
    const std::vector<ID3D12Resource*>& list = g_framegen_images[swapchain];
    *count = static_cast<uint32_t>(list.size());
    if (capacity == 0) return XR_SUCCESS;
    if (capacity < list.size()) return XR_ERROR_SIZE_INSUFFICIENT;
    auto* d3d12_images = reinterpret_cast<XrSwapchainImageD3D12KHR*>(images);
    for (std::size_t i = 0; i < list.size(); ++i) d3d12_images[i].texture = list[i];
    return XR_SUCCESS;
}

// The raw depth `encoding` stores for `meters` (1/z linear from minDepth at
// nearZ to maxDepth at farZ, either of them infinitely far).
float RawDepth(const depthxr::FrameGenDepthEncoding& encoding, double meters) {
    const auto inverse = [](double z) { return std::isfinite(z) && z > 0.0 ? 1.0 / z : 0.0; };
    const double near_inverse = inverse(encoding.near_z);
    const double normalized = (1.0 / meters - near_inverse) / (inverse(encoding.far_z) - near_inverse);
    return static_cast<float>(encoding.min_depth + normalized * (encoding.max_depth - encoding.min_depth));
}

// Frame Generation reads a bridged game's depth: the game clears its depth to
// a plane `meters` away as its encoding stores it, the bridge mirrors it into
// the runtime's image at release, and Frame Generation's Direct3D 12 backend
// (on the bridge's device and queue, as in a session) captures it there and
// decodes it to that distance. Its depth check is locked to the depth as
// described: what it decides is the backend's own tests', not the bridge's.
void TestDepthReachesFrameGen(Game& game) {
    struct Case {
        const char* name;
        DXGI_FORMAT format;
        depthxr::FrameGenDepthEncoding encoding;
        double meters;
        double tolerance; // relative
    };
    depthxr::FrameGenDepthEncoding uevr; // reversed, infinitely far plane at 0, near plane 10 cm at 1
    uevr.near_z = std::numeric_limits<float>::max();
    uevr.far_z = 0.1f;
    depthxr::FrameGenDepthEncoding iracing; // standard, 5 cm to 1 km
    iracing.near_z = 0.05f;
    iracing.far_z = 1000.0f;
    depthxr::FrameGenDepthEncoding reversed; // reversed and finite
    reversed.near_z = 2000.0f;
    reversed.far_z = 0.2f;
    const Case cases[] = {
        {"UEVR (D32S8, reversed, infinite)", DXGI_FORMAT_D32_FLOAT_S8X24_UINT, uevr, 2.0, 1e-3},
        {"iRacing (D16, standard)", DXGI_FORMAT_D16_UNORM, iracing, 5.0, 0.02},
        {"D24S8 (reversed, finite)", DXGI_FORMAT_D24_UNORM_S8_UINT, reversed, 12.0, 1e-3},
    };
    constexpr std::uint32_t kSize = 64;
    for (const Case& test : cases) {
        D3D11Bridge bridge;
        Session session(game.device.Get());
        BeginBridge(game, bridge, session);
        ID3D12Device* device = BridgeDevice(bridge);
        std::string error;
        auto gpu = depthxr::CreateFrameGenGpu(bridge.Binding(), error);
        Check(gpu != nullptr && gpu->SupportsDepth(), std::string(test.name) + ": Frame Generation's backend: " + error);
        gpu->LockDepthForTest(depthxr::FrameGenDepthReading::kAsSubmitted, false, 1.0f);

        // The runtime's depth swapchain (the bridge's), a colour source and
        // Frame Generation's output.
        MakeRuntimeDepthImages(device, 2, kSize, kSize, test.format, 1);
        const XrSwapchain depth = Handle(0x70);
        std::string reason;
        Check(bridge.AddSwapchain(depth, DepthSwapchainInfo(test.format, kSize, kSize, 1), FakeEnumerate, reason),
              std::string(test.name) + ": bridged: " + reason);
        std::vector<ComPtr<ID3D12Resource>> runtime_depth = g_runtime_images;
        MakeRuntimeImages(device, 1, kSize, kSize, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
        std::vector<ComPtr<ID3D12Resource>> source = g_runtime_images;
        const std::int64_t output_format = gpu->OutputFormats(depthxr::FrameGenOutputKind::kSrgb8).front();
        MakeRuntimeImages(device, 1, kSize, kSize, static_cast<DXGI_FORMAT>(output_format));
        std::vector<ComPtr<ID3D12Resource>> output = g_runtime_images;
        const XrSwapchain source_chain = Handle(0x71);
        const XrSwapchain output_chain = Handle(0x72);
        g_framegen_images = {{depth, {runtime_depth[0].Get(), runtime_depth[1].Get()}},
                             {source_chain, {source[0].Get()}},
                             {output_chain, {output[0].Get()}}};
        for (const XrSwapchain chain : {depth, source_chain, output_chain}) {
            Check(gpu->Track(chain, FrameGenEnumerate, error), std::string(test.name) + ": track: " + error);
        }

        const XrRect2Di rect{{0, 0}, {static_cast<int>(kSize), static_cast<int>(kSize)}};
        depthxr::FrameGenViewSubmission view;
        view.source = {source_chain, 0, 0, rect, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB};
        view.outputs = {{output_chain, 0, 0, rect, output_format}};
        view.request.extent = {kSize, kSize};
        view.request.output = depthxr::FrameGenOutputKind::kSrgb8;
        for (auto* geometry : {&view.request.newest, &view.request.previous}) {
            geometry->angle_left = geometry->angle_down = -std::atan(1.0f);
            geometry->angle_right = geometry->angle_up = std::atan(1.0f);
        }
        depthxr::FrameGenTuning tuning;
        depthxr::FrameGenExecuteResult result;
        // A first frame sets the view up: nothing was captured for it, so it
        // is not generated from ("not captured when the game released it").
        const bool primed = gpu->Execute({view}, tuning, result, error);
        Check(!primed, std::string(test.name) + ": the first frame only sets the view up");
        // The game's frames: colour captured at release, its depth cleared
        // to the plane, released (the bridge's draw), then captured as the
        // layer captures a release; generated from once a frame came before.
        const float raw = RawDepth(test.encoding, test.meters);
        view.request.depth = true;
        view.request.synthesize = true;
        for (std::uint32_t frame = 0; frame < 3; ++frame) {
            const bool colour = gpu->Capture(0, view.source, view.request, tuning.quality, error);
            Check(colour, std::string(test.name) + ": colour captured at release: " + error);
            const std::uint32_t index = frame % 2;
            bridge.Acquired(depth, index);
            D3D11_DEPTH_STENCIL_VIEW_DESC target_desc{};
            target_desc.Format = test.format;
            target_desc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
            target_desc.Texture2DArray.ArraySize = 1;
            ComPtr<ID3D11DepthStencilView> target;
            Check(SUCCEEDED(game.device->CreateDepthStencilView(bridge.Images(depth)[index], &target_desc, &target)),
                  "a depth view");
            game.context->ClearDepthStencilView(target.Get(), D3D11_CLEAR_DEPTH, raw, 0);
            Check(bridge.Released(depth), std::string(test.name) + ": the release is copied");
            const bool captured =
                gpu->CaptureDepth(0, {depth, index, 0, rect, test.format}, test.encoding, view.request, error);
            Check(captured, std::string(test.name) + ": Frame Generation captures the runtime's depth: " + error);
            const bool executed = gpu->Execute({view}, tuning, result, error);
            Check(executed, std::string(test.name) + ": execute: " + error);
        }
        Check(result.depth.size() == 1 && result.depth[0], std::string(test.name) + ": the frame used the depth");
        // UEVR places its depth rect by its colour image's size: in a smaller
        // depth image (an upscaler's internal resolution) it reaches past
        // the image, and is refused rather than read as zeros.
        const XrRect2Di past{{static_cast<int>(kSize / 2), 0}, {static_cast<int>(kSize), static_cast<int>(kSize)}};
        const bool outside = gpu->CaptureDepth(0, {depth, 0, 0, past, test.format}, test.encoding, view.request, error);
        Check(!outside && Contains(error, "lies outside its 64x64 image"),
              std::string(test.name) + ": a depth rect past its image is refused: " + error);

        std::vector<float> linear;
        depthxr::FrameGenExtent extent{};
        std::uint32_t channels = 0;
        Check(gpu->ReadTexture(0, {depthxr::FrameGenTextureKind::kDepth, 0}, linear, extent, channels) &&
                  channels == 1 && !linear.empty(),
              std::string(test.name) + ": Frame Generation's linear depth read back");
        double worst = 0.0;
        for (const float meters : linear) {
            worst = std::max(worst, std::abs(meters - test.meters) / test.meters);
        }
        std::cout << test.name << ": raw " << raw << " decoded to " << linear[linear.size() / 2] << " m (plane "
                  << test.meters << " m, worst " << worst * 100.0 << "% off over " << extent.width << "x"
                  << extent.height << ")\n";
        Check(worst <= test.tolerance, std::string(test.name) + ": Frame Generation decodes the bridged depth to " +
                                           std::to_string(linear[linear.size() / 2]) + " m, the plane is " +
                                           std::to_string(test.meters) + " m");

        Check(gpu->WaitIdle(), std::string(test.name) + ": Frame Generation's work finishes");
        for (const XrSwapchain chain : {depth, source_chain, output_chain}) gpu->Forget(chain);
        gpu.reset();
        Check(bridge.WaitIdle(10000), std::string(test.name) + ": the bridge's queue finishes");
        CheckClean(bridge, game, std::string("depth to Frame Generation ") + test.name);
        Check(bridge.RemoveSwapchain(depth), std::string(test.name) + ": removed");
        bridge.End();
    }
    g_framegen_images.clear();
}

// BRIDGE_TEST_DEPTH_PERF=1 (`hw`): what mirroring a game's depth costs at
// games' sizes, on the GPU, per release: the Direct3D 11 copy into the twin
// (timestamps around the release on the game's context), and the time from
// just before the release to the runtime's image drawn on the bridge's queue
// (which waits for that copy first). Nothing is held back: each release
// finishes before the next.
void MeasureDepthCost(Game& game) {
    struct Size {
        const char* name;
        DXGI_FORMAT format;
        std::uint32_t width, height, slices;
    };
    const Size sizes[] = {
        {"iRacing (D16 6904x3184)", DXGI_FORMAT_D16_UNORM, 6904, 3184, 1},
        {"iRacing's size as D32 (6904x3184)", DXGI_FORMAT_D32_FLOAT, 6904, 3184, 1},
        {"UEVR double wide (D32S8 5120x2560)", DXGI_FORMAT_D32_FLOAT_S8X24_UINT, 5120, 2560, 1},
        {"two slices (D24S8 2x2560x2560)", DXGI_FORMAT_D24_UNORM_S8_UINT, 2560, 2560, 2},
    };
    for (const Size& size : sizes) {
        D3D11Bridge bridge;
        Session session(game.device.Get());
        BeginBridge(game, bridge, session);
        ID3D12Device* device = BridgeDevice(bridge);
        ID3D12CommandQueue* queue = BridgeQueue(bridge);
        MakeRuntimeDepthImages(device, 3, size.width, size.height, size.format, size.slices);
        const XrSwapchain swapchain = Handle(0x80);
        std::string reason;
        Check(bridge.AddSwapchain(swapchain, DepthSwapchainInfo(size.format, size.width, size.height, size.slices),
                                  FakeEnumerate, reason),
              std::string(size.name) + ": bridged: " + reason);
        const std::vector<ID3D11Texture2D*> images = bridge.Images(swapchain);

        D3D11_QUERY_DESC disjoint_desc{D3D11_QUERY_TIMESTAMP_DISJOINT};
        D3D11_QUERY_DESC stamp_desc{D3D11_QUERY_TIMESTAMP};
        ComPtr<ID3D11Query> disjoint, before11, after11;
        Check(SUCCEEDED(game.device->CreateQuery(&disjoint_desc, &disjoint)) &&
                  SUCCEEDED(game.device->CreateQuery(&stamp_desc, &before11)) &&
                  SUCCEEDED(game.device->CreateQuery(&stamp_desc, &after11)),
              "Direct3D 11 timestamps");
        D3D12_QUERY_HEAP_DESC heap_desc{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 2, 0};
        ComPtr<ID3D12QueryHeap> stamps;
        ComPtr<ID3D12Resource> stamp_buffer;
        D3D12_HEAP_PROPERTIES readback{D3D12_HEAP_TYPE_READBACK};
        D3D12_RESOURCE_DESC buffer_desc{};
        buffer_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer_desc.Width = 16;
        buffer_desc.Height = buffer_desc.DepthOrArraySize = buffer_desc.MipLevels = 1;
        buffer_desc.SampleDesc.Count = 1;
        buffer_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12CommandAllocator> allocators[2];
        ComPtr<ID3D12GraphicsCommandList> lists[2];
        Check(SUCCEEDED(device->CreateQueryHeap(&heap_desc, IID_PPV_ARGS(&stamps))) &&
                  SUCCEEDED(device->CreateCommittedResource(&readback, D3D12_HEAP_FLAG_NONE, &buffer_desc,
                                                            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                            IID_PPV_ARGS(&stamp_buffer))),
              "Direct3D 12 timestamps");
        for (int i = 0; i < 2; ++i) {
            Check(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocators[i]))) &&
                      SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators[i].Get(), nullptr,
                                                          IID_PPV_ARGS(&lists[i]))),
                  "timestamp commands");
            lists[i]->EndQuery(stamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, i);
            if (i == 1) lists[i]->ResolveQueryData(stamps.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, stamp_buffer.Get(), 0);
            lists[i]->Close();
        }
        UINT64 frequency12 = 0;
        queue->GetTimestampFrequency(&frequency12);

        // The GPU's clocks up first: an idle GPU's first milliseconds of work
        // run several times slower.
        for (int release = 0; release < 90; ++release) {
            bridge.Acquired(swapchain, static_cast<std::uint32_t>(release) % 3);
            Check(bridge.Released(swapchain), std::string(size.name) + ": warmed");
        }
        Check(bridge.WaitIdle(10000) && GameRuns(game), std::string(size.name) + ": warm-up finished");

        std::vector<double> copy_ms, ready_ms;
        for (int iteration = 0; iteration < 40; ++iteration) {
            const std::uint32_t index = static_cast<std::uint32_t>(iteration) % 3;
            bridge.Acquired(swapchain, index);
            for (std::uint32_t slice = 0; slice < size.slices; ++slice) {
                D3D11_DEPTH_STENCIL_VIEW_DESC view{};
                view.Format = size.format;
                view.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
                view.Texture2DArray.FirstArraySlice = slice;
                view.Texture2DArray.ArraySize = 1;
                ComPtr<ID3D11DepthStencilView> target;
                Check(SUCCEEDED(game.device->CreateDepthStencilView(images[index], &view, &target)), "a depth view");
                game.context->ClearDepthStencilView(target.Get(), D3D11_CLEAR_DEPTH, 0.25f + 0.01f * iteration, 0);
            }
            Check(GameRuns(game), "the game's clear finishes");
            game.context->Begin(disjoint.Get());
            game.context->End(before11.Get());
            ID3D12CommandList* first[] = {lists[0].Get()};
            queue->ExecuteCommandLists(1, first);
            Check(bridge.Released(swapchain), std::string(size.name) + ": copied");
            game.context->End(after11.Get());
            game.context->End(disjoint.Get());
            game.context->Flush(); // the end stamp right after the copy, not at a later flush
            ID3D12CommandList* second[] = {lists[1].Get()};
            queue->ExecuteCommandLists(1, second);
            Check(bridge.WaitIdle(10000) && GameRuns(game), std::string(size.name) + ": finished");
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT timing{};
            UINT64 begin11 = 0, end11 = 0;
            while (game.context->GetData(disjoint.Get(), &timing, sizeof(timing), 0) != S_OK) Sleep(0);
            game.context->GetData(before11.Get(), &begin11, sizeof(begin11), 0);
            game.context->GetData(after11.Get(), &end11, sizeof(end11), 0);
            void* mapped = nullptr;
            Check(SUCCEEDED(stamp_buffer->Map(0, nullptr, &mapped)), "map the timestamps");
            UINT64 stamps12[2]{};
            std::memcpy(stamps12, mapped, sizeof(stamps12));
            stamp_buffer->Unmap(0, nullptr);
            if (iteration >= 5 && !timing.Disjoint) {
                copy_ms.push_back(1000.0 * static_cast<double>(end11 - begin11) / static_cast<double>(timing.Frequency));
                ready_ms.push_back(1000.0 * static_cast<double>(stamps12[1] - stamps12[0]) /
                                   static_cast<double>(frequency12));
            }
        }
        const auto median = [](std::vector<double> values) {
            std::sort(values.begin(), values.end());
            return values.empty() ? -1.0 : values[values.size() / 2];
        };
        const auto worst = [](const std::vector<double>& values) {
            return values.empty() ? -1.0 : *std::max_element(values.begin(), values.end());
        };
        // Back to back, as a session releases one image after another: the
        // bridge's queue's time from before the first release to the last
        // drawn, per release (the copies and draws of releases overlapping as
        // they may).
        constexpr int kBackToBack = 30;
        ID3D12CommandList* first[] = {lists[0].Get()};
        queue->ExecuteCommandLists(1, first);
        for (int release = 0; release < kBackToBack; ++release) {
            bridge.Acquired(swapchain, static_cast<std::uint32_t>(release) % 3);
            Check(bridge.Released(swapchain), std::string(size.name) + ": copied back to back");
        }
        ID3D12CommandList* second[] = {lists[1].Get()};
        queue->ExecuteCommandLists(1, second);
        Check(bridge.WaitIdle(10000) && GameRuns(game), std::string(size.name) + ": back to back finished");
        void* mapped = nullptr;
        Check(SUCCEEDED(stamp_buffer->Map(0, nullptr, &mapped)), "map the timestamps");
        UINT64 span[2]{};
        std::memcpy(span, mapped, sizeof(span));
        stamp_buffer->Unmap(0, nullptr);
        const double per_release =
            1000.0 * static_cast<double>(span[1] - span[0]) / static_cast<double>(frequency12) / kBackToBack;
        std::cout << "depth cost " << size.name << ": Direct3D 11 copy " << median(copy_ms) << " ms (max "
                  << worst(copy_ms) << "), release to runtime image drawn " << median(ready_ms) << " ms (max "
                  << worst(ready_ms) << "), back to back " << per_release << " ms a release, "
                  << bridge.Describe(swapchain) << '\n';
        CheckClean(bridge, game, std::string("depth cost ") + size.name);
        Check(bridge.RemoveSwapchain(swapchain), std::string(size.name) + ": removed");
        bridge.End();
    }
}

// A runtime image that does not match is refused, with the reason.
void TestMismatch(Game& game) {
    D3D11Bridge bridge;
    Session session(game.device.Get());
    BeginBridge(game, bridge, session);
    MakeRuntimeImages(BridgeDevice(bridge), 2, 30, 32, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    std::string reason;
    Check(!bridge.AddSwapchain(Handle(0x60), ColorInfo(32, 32), FakeEnumerate, reason) &&
              Contains(reason, "does not match"),
          "a mismatched runtime image is refused: " + reason);
    Check(!bridge.Bridged(Handle(0x60)), "not bridged after a refusal");
    // A colour image the runtime did not make a render target: the copy's
    // transitions would be invalid.
    MakeRuntimeImages(BridgeDevice(bridge), 2, 32, 32, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, 1, false);
    Check(!bridge.AddSwapchain(Handle(0x61), ColorInfo(32, 32), FakeEnumerate, reason) &&
              Contains(reason, "not a render target"),
          "a runtime image that is no render target is refused: " + reason);
    CheckClean(bridge, game, "mismatch");
    bridge.End();
}

// Composed swapchains (Quadviews' views) are flagged for the caller; they are
// copied like any other.
void TestComposed(Game& game) {
    D3D11Bridge bridge;
    Session session(game.device.Get());
    BeginBridge(game, bridge, session);
    ID3D12Device* device = BridgeDevice(bridge);
    MakeRuntimeImages(device, 3, 32, 32, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    const XrSwapchain swapchain = Handle(0x70);
    std::string reason;
    Check(bridge.AddSwapchain(swapchain, ColorInfo(32, 32), FakeEnumerate, reason), "bridged: " + reason);
    Check(!bridge.Composed(swapchain), "not composed until the layer says so");
    bridge.SetComposed(swapchain, true);
    Check(bridge.Composed(swapchain), "composed");
    bridge.SetComposed(Handle(0x71), true); // not bridged: ignored
    Check(!bridge.Composed(Handle(0x71)), "only bridged swapchains are composed");
    const std::array<std::uint8_t, 4> color{33, 66, 99, 255};
    bridge.Acquired(swapchain, 2);
    Clear(game, bridge.Images(swapchain)[2], color);
    bridge.Released(swapchain);
    Check(bridge.WaitIdle(10000), "the bridge's queue finishes");
    Check(Near(ReadRuntimeImage(device, 2), color), "a composed release reaches the runtime too");
    bridge.SetComposed(swapchain, false);
    Check(!bridge.Composed(swapchain), "no longer composed");
    CheckClean(bridge, game, "composed");
    bridge.End();
}

// Releases without acquires are counted, not copied.
void TestUnmatched(Game& game) {
    D3D11Bridge bridge;
    Session session(game.device.Get());
    BeginBridge(game, bridge, session);
    MakeRuntimeImages(BridgeDevice(bridge), 2, 16, 16, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    const XrSwapchain swapchain = Handle(0x80);
    std::string reason;
    Check(bridge.AddSwapchain(swapchain, ColorInfo(16, 16), FakeEnumerate, reason), "bridged: " + reason);
    bridge.Released(swapchain);
    bridge.Acquired(swapchain, 1);
    bridge.Abandoned(swapchain);
    const std::string summary = bridge.Summary();
    Check(SummaryValue(summary, "unmatchedReleases") == 1 && SummaryValue(summary, "copies") == 0,
          "a release with nothing acquired is counted; an abandoned one is not copied: " + summary);
    CheckClean(bridge, game, "unmatched");
    bridge.End();
}

// A stalled bridge queue: a destroyed swapchain whose copies did not finish
// is kept (the runtime's must be too), and End gives up after its wait,
// Direct3D 11 running on; once the queue resumes, its copies land in the
// runtime's images (still referenced) without removing the device.
void TestStalledQueue(Game& game) {
    D3D11Bridge bridge;
    Session session(game.device.Get());
    BeginBridge(game, bridge, session);
    ComPtr<ID3D12Device> device = BridgeDevice(bridge);
    MakeRuntimeImages(device.Get(), 2, 32, 32, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    const XrSwapchain swapchain = Handle(0x90);
    std::string reason;
    Check(bridge.AddSwapchain(swapchain, ColorInfo(32, 32), FakeEnumerate, reason), "bridged: " + reason);
    Gate gate(device.Get(), game);
    gate.HoldQueue(BridgeQueue(bridge));
    const std::array<std::uint8_t, 4> color{10, 220, 220, 255};
    bridge.Acquired(swapchain, 0);
    Clear(game, bridge.Images(swapchain)[0], color);
    bridge.Released(swapchain);
    bridge.Acquired(swapchain, 0); // a Direct3D 11 wait for the stalled copy
    auto start = std::chrono::steady_clock::now();
    Check(!bridge.RemoveSwapchain(swapchain), "copies into the destroyed swapchain did not finish");
    Check(SecondsSince(start) < 3.0, "RemoveSwapchain gives up after its wait");
    Check(Contains(bridge.Summary(), "(0 live, 1 kept)") && g_log.size() == 1, "the swapchain is kept, and reported");
    start = std::chrono::steady_clock::now();
    bridge.End();
    Check(SecondsSince(start) < 3.0, "End gives up after its wait: " + std::to_string(SecondsSince(start)) + " s");
    Check(g_log.size() == 2 && Contains(g_log.back(), "did not finish at teardown"), "End reports the stalled queue");
    Check(GameRuns(game), "Direct3D 11 runs on (its wait for the stalled copy ended)");
    gate.Open();
    Sleep(200);
    Check(device->GetDeviceRemovedReason() == S_OK, "the queue's late copies remove nothing");
    Check(Near(ReadRuntimeImage(device.Get(), 0), color), "the late copy landed in the runtime's image");
    ValidationErrors(device.Get(), game); // the stall's own messages
}

// The bridge's queue stalled while one image is released again and again:
// once every recording of its copy is queued, a release waits a while, then
// goes uncopied and says so (Frame Generation must not capture it).
void TestSkippedCopy(Game& game) {
    D3D11Bridge bridge;
    Session session(game.device.Get());
    BeginBridge(game, bridge, session);
    ID3D12Device* device = BridgeDevice(bridge);
    MakeRuntimeImages(device, 1, 16, 16, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    const XrSwapchain swapchain = Handle(0xa8);
    std::string reason;
    Check(bridge.AddSwapchain(swapchain, ColorInfo(16, 16), FakeEnumerate, reason), "bridged: " + reason);
    Gate gate(device, game);
    gate.HoldQueue(BridgeQueue(bridge));
    for (int release = 0; release < 4; ++release) {
        bridge.Acquired(swapchain, 0);
        Check(bridge.Released(swapchain), "release " + std::to_string(release) + " is copied (a recording is free)");
    }
    bridge.Acquired(swapchain, 0);
    const auto start = std::chrono::steady_clock::now();
    Check(!bridge.Released(swapchain), "with every recording queued, the release is not copied");
    Check(SecondsSince(start) < 1.0, "after a bounded wait");
    const std::string summary = bridge.Summary();
    Check(SummaryValue(summary, "skippedCopies") == 1 && SummaryValue(summary, "copyRecordings") == 4,
          "one skipped copy, four recordings: " + summary);
    Check(g_log.size() == 1 && Contains(g_log[0], "stalled"), "the stall is reported");
    gate.Open();
    Check(bridge.WaitIdle(10000), "the bridge's queue finishes");
    bridge.Acquired(swapchain, 0);
    Check(bridge.Released(swapchain), "copies resume once the queue moves");
    Check(bridge.WaitIdle(10000), "the bridge's queue finishes");
    g_log.clear();
    CheckClean(bridge, game, "skipped copy");
    bridge.End();
}

// Direct3D 11 never signals what the bridge's queue waits for: Drain gets the
// queue through anyway, before the runtime's images go.
void TestDrainPastGame(Game& game) {
    D3D11Bridge bridge;
    Session session(game.device.Get());
    BeginBridge(game, bridge, session);
    ID3D12Device* device = BridgeDevice(bridge);
    MakeRuntimeImages(device, 2, 32, 32, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    const XrSwapchain swapchain = Handle(0xa0);
    std::string reason;
    Check(bridge.AddSwapchain(swapchain, ColorInfo(32, 32), FakeEnumerate, reason), "bridged: " + reason);
    Gate gate(device, game);
    bridge.Acquired(swapchain, 1);
    gate.HoldGame(game);
    Clear(game, bridge.Images(swapchain)[1], {1, 2, 3, 255});
    bridge.Released(swapchain);
    const auto start = std::chrono::steady_clock::now();
    Check(bridge.Drain(), "Drain gets the bridge's queue through");
    const double seconds = SecondsSince(start);
    Check(seconds > 1.5 && seconds < 5.0, "after its first wait: " + std::to_string(seconds) + " s");
    Check(g_log.empty(), "nothing to report when the queue finished");
    gate.Open(); // WARP keeps a texture with pending Direct3D 11 work from going
    Check(GameRuns(game), "Direct3D 11 runs on");
    ValidationErrors(device, game);
    bridge.End();
}

// The game's frames end on one thread and its images are released on
// another: the bridge's Direct3D 11 calls would share the immediate context
// across threads, so the device's multithread protection goes on (reported).
void TestSecondThread(Game& game) {
    ComPtr<ID3D11Multithread> multithread;
    Check(SUCCEEDED(game.context.As(&multithread)), "the game's context has ID3D11Multithread");
    multithread->SetMultithreadProtected(FALSE);
    D3D11Bridge bridge;
    Session session(game.device.Get());
    BeginBridge(game, bridge, session);
    MakeRuntimeImages(BridgeDevice(bridge), 2, 16, 16, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    const XrSwapchain swapchain = Handle(0xc0);
    std::string reason;
    Check(bridge.AddSwapchain(swapchain, ColorInfo(16, 16), FakeEnumerate, reason), "bridged: " + reason);
    bridge.FrameEnded(); // the frame's thread
    bridge.FrameEnded();
    Check(g_log.empty() && !multithread->GetMultithreadProtected(), "one thread: nothing changes");
    bridge.Acquired(swapchain, 0);
    std::thread release([&] { bridge.Released(swapchain); });
    release.join();
    Check(!multithread->GetMultithreadProtected() && g_log.empty(),
          "the second thread's call itself does not turn protection on (it waits for the device's lock)");
    bridge.StageReleases(XR_NULL_HANDLE); // the next hand-over, before the layer's locks
    Check(multithread->GetMultithreadProtected() != FALSE, "a second thread turns multithread protection on");
    Check(g_log.size() == 1 && Contains(g_log[0], "turned the game device's multithread protection on"),
          "and says so");
    Check(Contains(bridge.Summary(), "secondThread=yes, multithreadProtected=yes"), "summary: " + bridge.Summary());
    std::thread again([&] { bridge.FrameEnded(); });
    again.join();
    Check(g_log.size() == 1, "reported once");
    Check(bridge.WaitIdle(10000), "the bridge's queue finishes");
    g_log.clear();
    CheckClean(bridge, game, "second thread");
    bridge.End();
    multithread->SetMultithreadProtected(FALSE);
}

// A colour-only game (DCS: no depth, protection off) whose second thread
// makes its first swapchain call while another thread holds the device's
// lock and then needs the bridge: the hand-over's context hold
// (StageReleases), or a game thread of its own. Turning protection on waits
// for that lock; done inside the second thread's call, holding the bridge's
// mutex, it deadlocked with the holder's next bridge call. Events drive it:
// a thread enters the lock, the second thread releases (ReleaseCalled, as
// the layer calls it), and the holder waits for that release (2 s), then
// calls StageReleases, which takes the context (its own lock again) and the
// bridge's mutex. Both must finish; a deadlock fails the test after 10 s
// (the process ends: its threads cannot be got back). Protection goes on in
// that StageReleases, reported once.
void TestSecondThreadUnderHeldContext(Game& game) {
    ComPtr<ID3D11Multithread> multithread;
    Check(SUCCEEDED(game.context.As(&multithread)), "the game's context has ID3D11Multithread");
    multithread->SetMultithreadProtected(FALSE);
    D3D11Bridge bridge;
    Session session(game.device.Get());
    BeginBridge(game, bridge, session);
    MakeRuntimeImages(BridgeDevice(bridge), 2, 16, 16, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    const XrSwapchain swapchain = Handle(0xc1);
    std::string reason;
    Check(bridge.AddSwapchain(swapchain, ColorInfo(16, 16), FakeEnumerate, reason), "bridged: " + reason);
    bridge.FrameEnded(); // the frame's thread (this one)
    bridge.Acquired(swapchain, 0);

    const auto await = [](HANDLE event, DWORD ms, const char* what) {
        if (WaitForSingleObject(event, ms) == WAIT_OBJECT_0) return true;
        if (what) {
            std::cout << "FAILED: second thread under a held context: " << what
                      << ": deadlocked (turning protection on waits for the device's lock, holding the bridge's "
                         "mutex the lock's holder waits for)\n"
                      << std::flush;
            TerminateProcess(GetCurrentProcess(), 1);
        }
        return false;
    };
    const HANDLE entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    const HANDLE released = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    const HANDLE staged = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::atomic<bool> released_while_held{false};
    std::thread holder([&] {
        multithread->Enter();
        SetEvent(entered);
        released_while_held = await(released, 2000, nullptr);
        bridge.StageReleases(XR_NULL_HANDLE);
        multithread->Leave();
        SetEvent(staged);
    });
    await(entered, 10000, "the holder did not enter the device's lock");
    std::thread second([&] {
        bridge.ReleaseCalled(swapchain);
        SetEvent(released);
    });
    await(released, 10000, "the second thread's release did not return");
    await(staged, 10000, "the holder's StageReleases did not return");
    holder.join();
    second.join();
    for (const HANDLE event : {entered, released, staged}) CloseHandle(event);
    Check(released_while_held, "the second thread's release returned while the device's lock was held");
    Check(multithread->GetMultithreadProtected() != FALSE, "the hand-over turned protection on");
    Check(g_log.size() == 1 && Contains(g_log[0], "turned the game device's multithread protection on"),
          "and said so, once");
    Check(bridge.Released(swapchain), "the release is handed over");
    Check(bridge.WaitIdle(10000), "the bridge's queue finishes");
    g_log.clear();
    CheckClean(bridge, game, "second thread under a held context");
    bridge.End();
    multithread->SetMultithreadProtected(FALSE);
}

// The bridge's device removed: the session is lost, and teardown does not wait.
// Last: Direct3D 12 makes one device per adapter in a process.
void TestDeviceLost(Game& game) {
    D3D11Bridge bridge;
    Session session(game.device.Get());
    BeginBridge(game, bridge, session);
    ComPtr<ID3D12Device5> device5;
    if (FAILED(BridgeDevice(bridge)->QueryInterface(IID_PPV_ARGS(&device5)))) {
        std::cout << "device loss not tested: no ID3D12Device5\n";
        bridge.End();
        return;
    }
    MakeRuntimeImages(device5.Get(), 2, 16, 16, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    const XrSwapchain swapchain = Handle(0xb0);
    std::string reason;
    Check(bridge.AddSwapchain(swapchain, ColorInfo(16, 16), FakeEnumerate, reason), "bridged: " + reason);
    bridge.Acquired(swapchain, 0);
    bridge.Released(swapchain);
    std::string lost;
    Check(!bridge.Lost(lost), "not lost yet");
    device5->RemoveDevice();
    Check(bridge.Lost(lost) && Contains(lost, "removed"), "the removal is seen: " + lost);
    bridge.Acquired(swapchain, 0); // the done fence reads past every value: no wait
    bridge.Released(swapchain);
    auto start = std::chrono::steady_clock::now();
    Check(bridge.Drain(), "a removed device counts as idle");
    const double drain = SecondsSince(start);
    start = std::chrono::steady_clock::now();
    bridge.End();
    const double end = SecondsSince(start);
    // Letting the objects of a removed device go takes about a second on WARP;
    // the bridge's own waits end at once.
    Check(drain < 1.0 && end < 2.0, "teardown does not wait on a removed device: Drain " + std::to_string(drain) +
                                      " s, End " + std::to_string(end) + " s");
    Check(GameRuns(game), "the game's Direct3D 11 device runs on");
}

} // namespace

int main(int argc, char** argv) {
    const char* no_stall = std::getenv("FG_TEST_NO_STALL");
    const bool skip_negative_controls = no_stall && std::string(no_stall) == "1";
    const bool hardware = argc > 1 && std::string(argv[1]) == "hw";
    // Before any Direct3D 12 device exists.
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
        debug->EnableDebugLayer();
    }
    Game game;
    if (!MakeGame(hardware, game)) {
        std::cout << "skipped: no Direct3D 11 device\n";
        return kSkip;
    }
    TestBegin(game);
    TestSessionGraphics(game, hardware);
    TestCreateInfo();
    TestTwinFrames(game);
    if (!skip_negative_controls) {
        TestWrittenOrdering(game);
        TestPendingRelease(game);
    } else {
        std::cout << "FG_TEST_NO_STALL=1: skipped artificial ordering and pending-release holds\n";
    }
    TestDoneWait(game);
    TestResolve(game);
    TestArray(game);
    TestDepthFormats(game);
    TestDepthStillBound(game);
    TestDepthKeepsGameState(game);
    TestDepthCopyHoldsContext(game);
    TestDepthCopyHoldsContext(game, false);
    TestDepthDoneWait(game);
    TestDepthPrivate(game);
    TestDepthReachesFrameGen(game);
    if (const char* perf = std::getenv("BRIDGE_TEST_DEPTH_PERF"); hardware && perf && std::string(perf) == "1") {
        MeasureDepthCost(game);
    }
    TestMismatch(game);
    TestComposed(game);
    TestUnmatched(game);
    if (!skip_negative_controls) {
        TestStalledQueue(game);
        TestSkippedCopy(game);
        TestDrainPastGame(game);
    }
    TestSecondThread(game);
    TestSecondThreadUnderHeldContext(game);
    if (!skip_negative_controls) {
        TestDeviceLost(game);
    } else {
        std::cout << "FG_TEST_NO_STALL=1: skipped stalled-queue and device-removal controls\n";
    }
    g_runtime_images.clear();
    std::cout << "d3d11 bridge tests passed (" << (hardware ? "hardware" : "WARP")
              << (debug ? ", D3D12 debug layer" : "") << (game.debug ? ", D3D11 debug layer" : "") << ")\n";
    return 0;
}
