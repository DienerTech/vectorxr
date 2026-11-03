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
#include "depthxr/session_graphics.h"
#include "test_adapter.h"
#include "test_gates.h"

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
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

// Depth reaches the runtime's depth images, every format value for value,
// every slice, every image its last release, with the runtime's images typed
// or typeless (PimaxXR's and Virtual Desktop's are the game's format, opened
// from Direct3D 11): drawn by the game, copied into the twin by Direct3D 11
// and drawn into the runtime's image by the bridge's queue.
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
    game.context->CSSetShader(nullptr, nullptr, 0);
    ID3D11ShaderResourceView* no_read = nullptr;
    ID3D11UnorderedAccessView* no_write = nullptr;
    game.context->CSSetShaderResources(0, 1, &no_read);
    game.context->CSSetUnorderedAccessViews(0, 1, &no_write, nullptr);
    CheckClean(bridge, game, "depth still bound");
    bridge.End();
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
    TestDepthDoneWait(game);
    TestDepthPrivate(game);
    TestMismatch(game);
    TestComposed(game);
    TestUnmatched(game);
    if (!skip_negative_controls) {
        TestStalledQueue(game);
        TestSkippedCopy(game);
        TestDrainPastGame(game);
    }
    TestSecondThread(game);
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
