// Probes Direct3D 11 <-> Direct3D 12 texture and fence sharing on the GPU, at the formats and sizes DCS
// submits, before the D3D11 bridge for Frame Generation is built on it. It needs real hardware (WARP shares
// nothing with a GPU device), so the build compiles it but CTest does not run it.
//
//   depthxr_bridge_probe [--debug] [--gpu-validation] [--timings] [--quick]
//                        [--feature-level-11-0] [--single-threaded] [--removal] [--teardown] [--json <path>]
//
//   --debug           Both APIs' debug layers; their warnings and errors are attributed to the case that
//                     caused them, and an unexpected error fails the run. Timings mean nothing with it on.
//   --gpu-validation  Also D3D12 GPU-based validation (implies --debug). It needs synchronized queue
//                     validation, which holds cross-API waits on the CPU, so the fence and ring cases are skipped.
//   --timings         GPU timings of copies, rendering and reads into shared against private textures.
//   --quick           Correctness at one small size only.
//   --feature-level-11-0, --single-threaded
//                     Create the D3D11 device at feature level 11_0 only, or SINGLETHREADED: DCS's own
//                     device is unrecorded, and either could change what shares.
//   --removal         Only remove the D3D12 device while D3D11 waits on it, and record what D3D11 sees.
//   --teardown        Only leave a D3D11 wait pending on a value D3D12 never signals: what releases it.
//   --json <path>     Write every record as JSON.
//
// Ownership lineages, each in two variants:
//   A  Direct3D 11 creates the texture and an NT handle; Direct3D 12 opens it.
//      A_keyed_mutex: MiscFlags SHARED_NTHANDLE | SHARED_KEYEDMUTEX (2304), the flags that ran in DCS.
//      A_nt_shared:   SHARED_NTHANDLE | SHARED, no mutex.
//   B  Direct3D 12 creates it on a shared heap; Direct3D 11 opens it.
//      B_simultaneous: ALLOW_SIMULTANEOUS_ACCESS.
//      B_plain:        without it.
// Cross-API order comes from shared fences that only one API signals each (D3D11 -> D3D12 "written", D3D12 ->
// D3D11 "done"), so every fence's value only rises however the two queues interleave. Only Direct3D 11
// touches the keyed mutex; Direct3D 12 never participates in it. CPU waits appear only after a case's GPU
// work is queued, to read results back.

#include <windows.h>
#include <d3d11_4.h>
#include <d3d11sdklayers.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <dxgidebug.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

constexpr DWORD kMutexTimeoutMs = 5000;
constexpr double kPollTimeoutMs = 2000.0;

// --- Options, records and output -------------------------------------------------------------------------------

struct Options {
    bool debug = false;
    bool gpu_validation = false;
    bool timings = false;
    bool quick = false;
    // DCS's own D3D11 device is unrecorded: these recreate the two variants that could change sharing.
    bool feature_level_11_0 = false;
    bool single_threaded = false;
    // Removes the D3D12 device mid-handoff and records what D3D11 sees. Irreversible, so it runs alone.
    bool removal = false;
    // Leaves a D3D11 wait pending on a value D3D12 never signals, then releases D3D12. May strand the D3D11
    // context for good, so it runs alone.
    bool teardown = false;
    std::string json_path;
};
Options g_options;

struct Size {
    uint32_t width;
    uint32_t height;
    const char* role;
};

// DCS quad views at 40% focus; VectorXR's unrounded focus recommendation; and the
// quad-views compositor's stereo output canvas from a Crystal Super tester run, the image Frame Generation
// would actually read with Quadviews on.
constexpr Size kPeripheral{2184, 2200, "dcs_peripheral"};
constexpr Size kFocus{2748, 2764, "dcs_focus_40"};
constexpr Size kUnrounded{2746, 2763, "unrounded"};
constexpr Size kCanvas{6240, 6280, "quadviews_canvas"};
constexpr Size kQuick{256, 144, "quick"};

struct Record {
    std::string section;
    std::string name;
    // pass | fail | expected_fail | unexpected_pass | unsupported | info | insensitive
    // (unsupported: the combination cannot be created or opened, which is a finding, not a probe failure)
    std::string status;
    std::string detail;
    std::vector<std::pair<std::string, double>> metrics;
    std::vector<std::string> debug_messages;
    std::size_t debug_message_count = 0;
    bool debug_error = false;
    // Invalid-on-purpose calls (an expected failure) are allowed to make the debug layers complain.
    bool expect_debug_errors = false;
};
std::vector<Record> g_records;

std::string Hex(uint64_t value, int digits = 8) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "0x%0*llX", digits, static_cast<unsigned long long>(value));
    return buffer;
}

std::string HrText(HRESULT hr) {
    return Hex(static_cast<uint32_t>(hr));
}

std::string SizeText(const Size& size) {
    return std::to_string(size.width) + "x" + std::to_string(size.height);
}

double Median(std::vector<double> values) {
    if (values.empty()) return std::numeric_limits<double>::quiet_NaN();
    std::sort(values.begin(), values.end());
    return values[values.size() / 2];
}

double Percentile(std::vector<double> values, double fraction) {
    if (values.empty()) return std::numeric_limits<double>::quiet_NaN();
    std::sort(values.begin(), values.end());
    const std::size_t index = std::min(values.size() - 1, static_cast<std::size_t>(fraction * values.size()));
    return values[index];
}

double NowMs() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

[[noreturn]] void Abort(const std::string& why) {
    std::cerr << "ABORT: " << why << '\n';
    std::exit(3);
}

void Must(HRESULT hr, const char* what) {
    if (FAILED(hr)) Abort(std::string(what) + " failed " + HrText(hr));
}

const char* FormatName(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return "R8G8B8A8_TYPELESS";
    case DXGI_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM";
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return "R8G8B8A8_UNORM_SRGB";
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return "B8G8R8A8_TYPELESS";
    case DXGI_FORMAT_B8G8R8A8_UNORM: return "B8G8R8A8_UNORM";
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return "B8G8R8A8_UNORM_SRGB";
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return "R16G16B16A16_TYPELESS";
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return "R16G16B16A16_FLOAT";
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return "R10G10B10A2_TYPELESS";
    case DXGI_FORMAT_R10G10B10A2_UNORM: return "R10G10B10A2_UNORM";
    case DXGI_FORMAT_B8G8R8X8_TYPELESS: return "B8G8R8X8_TYPELESS";
    case DXGI_FORMAT_B8G8R8X8_UNORM: return "B8G8R8X8_UNORM";
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: return "B8G8R8X8_UNORM_SRGB";
    case DXGI_FORMAT_R16G16_FLOAT: return "R16G16_FLOAT";
    case DXGI_FORMAT_R32_FLOAT: return "R32_FLOAT";
    case DXGI_FORMAT_R32_TYPELESS: return "R32_TYPELESS";
    case DXGI_FORMAT_R32G8X24_TYPELESS: return "R32G8X24_TYPELESS";
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: return "D32_FLOAT_S8X24_UINT";
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS: return "R32_FLOAT_X8X24_TYPELESS";
    case DXGI_FORMAT_R24G8_TYPELESS: return "R24G8_TYPELESS";
    case DXGI_FORMAT_D24_UNORM_S8_UINT: return "D24_UNORM_S8_UINT";
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS: return "R24_UNORM_X8_TYPELESS";
    case DXGI_FORMAT_D32_FLOAT: return "D32_FLOAT";
    case DXGI_FORMAT_R32G32B32A32_FLOAT: return "R32G32B32A32_FLOAT";
    default: return "format";
    }
}

uint32_t BytesPerPixel(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R32G8X24_TYPELESS:
        return 8;
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
        return 16;
    default:
        return 4;
    }
}

bool IsHalfFloat(DXGI_FORMAT format) {
    return format == DXGI_FORMAT_R16G16B16A16_TYPELESS || format == DXGI_FORMAT_R16G16B16A16_FLOAT;
}

// The X byte of B8G8R8X8 holds nothing; comparisons ignore it.
bool HasUndefinedAlpha(DXGI_FORMAT format) {
    return format == DXGI_FORMAT_B8G8R8X8_TYPELESS || format == DXGI_FORMAT_B8G8R8X8_UNORM ||
           format == DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;
}

std::string JsonEscape(std::string_view text) {
    std::string out;
    out.reserve(text.size() + 8);
    for (const char c : text) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char buffer[8];
                std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
                out += buffer;
            } else {
                out += c;
            }
        }
    }
    return out;
}

// --- Devices ---------------------------------------------------------------------------------------------------

struct Gpu {
    ComPtr<IDXGIAdapter1> adapter;
    DXGI_ADAPTER_DESC1 adapter_desc{};
    LARGE_INTEGER driver_version{};
    D3D_FEATURE_LEVEL level11{};
    ComPtr<ID3D11Device5> device11;
    ComPtr<ID3D11DeviceContext4> context11;
    ComPtr<ID3D11InfoQueue> info11;
    ComPtr<ID3D12Device> device12;
    ComPtr<ID3D12CommandQueue> queue12;
    ComPtr<ID3D12InfoQueue> info12;
    ComPtr<IDXGIInfoQueue> info_dxgi;

    // Command lists come from a pool of allocators, each retired by a value of the idle fence, so a run of
    // submissions never has to wait on the CPU to recycle one. `recycle_waits` counts the times it had to.
    static constexpr std::size_t kAllocators = 96;
    std::array<ComPtr<ID3D12CommandAllocator>, kAllocators> allocators;
    std::array<uint64_t, kAllocators> allocator_retire{};
    std::size_t next_allocator = 0;
    std::size_t open_allocator = 0;
    ComPtr<ID3D12GraphicsCommandList> list12;
    ComPtr<ID3D12Fence> idle12;
    uint64_t idle_value = 0;
    HANDLE idle_event = nullptr;
    uint64_t recycle_waits = 0;

    // Shader-visible SRV/UAV heap (two descriptors per dispatch) and a render-target heap; both are reset
    // between cases once the GPU is idle.
    ComPtr<ID3D12DescriptorHeap> srv_heap;
    UINT srv_increment = 0;
    UINT srv_next = 0;
    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    UINT rtv_increment = 0;
    UINT rtv_next = 0;

    ComPtr<ID3D12RootSignature> root_signature;
    ComPtr<ID3D12PipelineState> decode_pso;
    ComPtr<ID3D12PipelineState> reduce_pso;
    ComPtr<ID3DBlob> fullscreen_vs_code;
    ComPtr<ID3DBlob> pattern_ps_code;
    std::vector<std::pair<DXGI_FORMAT, ComPtr<ID3D12PipelineState>>> pattern_psos;
    ComPtr<ID3D12QueryHeap> timestamps12;
    ComPtr<ID3D12Resource> timestamp_results12;
    // 256 MiB scratch buffers whose copy evicts the GPU's L2 (96 MiB on an RTX 5090) before a cold sample.
    ComPtr<ID3D11Buffer> evict11_a;
    ComPtr<ID3D11Buffer> evict11_b;
    ComPtr<ID3D12Resource> evict12_a;
    ComPtr<ID3D12Resource> evict12_b;

    ComPtr<ID3D11VertexShader> fullscreen_vs;
    ComPtr<ID3D11PixelShader> pattern_ps;
    ComPtr<ID3D11PixelShader> depth_ps;
    ComPtr<ID3D11PixelShader> read_ps;
    ComPtr<ID3D11ComputeShader> decode_cs;
    ComPtr<ID3D11ComputeShader> depth_copy_cs;
    ComPtr<ID3D11ComputeShader> reduce_cs;
    ComPtr<ID3D11Buffer> constants11;
    ComPtr<ID3D11RasterizerState> rasterizer;
    ComPtr<ID3D11DepthStencilState> depth_always;
    ComPtr<ID3D11BlendState> blend_alpha;

    // D3D12 objects stay alive until the GPU is idle at the end of the case that queued them.
    std::vector<ComPtr<IUnknown>> graveyard;
};
Gpu g;

template <typename T>
T* Keep(const ComPtr<T>& object) {
    if (object) g.graveyard.push_back(object);
    return object.Get();
}

// A fence D3D12 creates and shares and D3D11 opens. Only one API signals each instance.
struct SharedFence {
    ComPtr<ID3D12Fence> fence12;
    ComPtr<ID3D11Fence> fence11;
    uint64_t value = 0;
};
SharedFence g_to12; // D3D11 signals "written", D3D12 queues wait
SharedFence g_to11; // D3D12 signals "done reading", D3D11 waits
SharedFence g_gate; // the CPU signals it, to hold one API back on purpose

void DrainDebug(Record& record) {
    constexpr std::size_t kKeep = 12;
    auto keep = [&record](const std::string& text, bool error) {
        record.debug_error |= error;
        ++record.debug_message_count;
        if (record.debug_messages.size() < kKeep) record.debug_messages.push_back(text);
    };
    if (g.info11) {
        const UINT64 count = g.info11->GetNumStoredMessages();
        for (UINT64 i = 0; i < count; ++i) {
            SIZE_T size = 0;
            if (FAILED(g.info11->GetMessage(i, nullptr, &size)) || size == 0) continue;
            std::vector<char> storage(size);
            auto* message = reinterpret_cast<D3D11_MESSAGE*>(storage.data());
            if (FAILED(g.info11->GetMessage(i, message, &size))) continue;
            if (message->Severity > D3D11_MESSAGE_SEVERITY_WARNING) continue;
            const bool error = message->Severity <= D3D11_MESSAGE_SEVERITY_ERROR;
            keep(std::string("d3d11 ") + (error ? "error" : "warning") + " #" + std::to_string(message->ID) + ": " +
                     (message->pDescription ? message->pDescription : ""),
                 error);
        }
        g.info11->ClearStoredMessages();
    }
    if (g.info12) {
        const UINT64 count = g.info12->GetNumStoredMessages();
        for (UINT64 i = 0; i < count; ++i) {
            SIZE_T size = 0;
            if (FAILED(g.info12->GetMessage(i, nullptr, &size)) || size == 0) continue;
            std::vector<char> storage(size);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            if (FAILED(g.info12->GetMessage(i, message, &size))) continue;
            if (message->Severity > D3D12_MESSAGE_SEVERITY_WARNING) continue;
            const bool error = message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR;
            keep(std::string("d3d12 ") + (error ? "error" : "warning") + " #" + std::to_string(message->ID) + ": " +
                     (message->pDescription ? message->pDescription : ""),
                 error);
        }
        g.info12->ClearStoredMessages();
    }
    if (g.info_dxgi) {
        const UINT64 count = g.info_dxgi->GetNumStoredMessages(DXGI_DEBUG_DXGI);
        for (UINT64 i = 0; i < count; ++i) {
            SIZE_T size = 0;
            if (FAILED(g.info_dxgi->GetMessage(DXGI_DEBUG_DXGI, i, nullptr, &size)) || size == 0) continue;
            std::vector<char> storage(size);
            auto* message = reinterpret_cast<DXGI_INFO_QUEUE_MESSAGE*>(storage.data());
            if (FAILED(g.info_dxgi->GetMessage(DXGI_DEBUG_DXGI, i, message, &size))) continue;
            if (message->Severity > DXGI_INFO_QUEUE_MESSAGE_SEVERITY_WARNING) continue;
            const bool error = message->Severity <= DXGI_INFO_QUEUE_MESSAGE_SEVERITY_ERROR;
            keep(std::string("dxgi ") + (error ? "error" : "warning") + " #" + std::to_string(message->ID) + ": " +
                     (message->pDescription ? message->pDescription : ""),
                 error);
        }
        g.info_dxgi->ClearStoredMessages(DXGI_DEBUG_DXGI);
    }
}

uint64_t Mark12();
void WaitIdle12();

void Finish(Record& record) {
    if (g_options.gpu_validation) WaitIdle12();
    DrainDebug(record);
    std::ostringstream line;
    line << "[" << record.status << "] " << record.section << " " << record.name;
    if (!record.detail.empty()) line << " | " << record.detail;
    for (const auto& [key, value] : record.metrics) {
        line << " " << key << "=" << value;
    }
    if (record.debug_message_count != 0) {
        line << " | debug: " << record.debug_message_count << " message(s)"
             << (record.debug_error ? (record.expect_debug_errors ? " incl. expected errors" : " INCL. ERRORS") : "");
        for (const std::string& message : record.debug_messages) line << "\n      " << message;
    }
    std::cout << line.str() << std::endl;
    g_records.push_back(std::move(record));
}

// Debug output raised between records (a case's teardown after its last record, or validation reported late)
// gets a record of its own, so it is never excused as another case's expected errors.
void DrainStray() {
    if (!g_options.debug) return;
    Record record{"debug", "between_records_after " +
                               (g_records.empty() ? std::string("start")
                                                  : g_records.back().section + " " + g_records.back().name)};
    DrainDebug(record);
    if (record.debug_message_count == 0) return;
    record.status = record.debug_error ? "fail" : "info";
    Finish(record);
}

// --- Direct3D 12 command plumbing ------------------------------------------------------------------------------

void WaitIdle12() {
    const uint64_t target = Mark12();
    if (g.idle12->GetCompletedValue() >= target) return;
    Must(g.idle12->SetEventOnCompletion(target, g.idle_event), "SetEventOnCompletion");
    if (WaitForSingleObject(g.idle_event, 30000) != WAIT_OBJECT_0) Abort("D3D12 queue did not go idle in 30 s");
}

ID3D12GraphicsCommandList* Begin12() {
    const std::size_t slot = g.next_allocator++ % Gpu::kAllocators;
    if (g.idle12->GetCompletedValue() < g.allocator_retire[slot]) {
        ++g.recycle_waits;
        Must(g.idle12->SetEventOnCompletion(g.allocator_retire[slot], g.idle_event), "SetEventOnCompletion");
        if (WaitForSingleObject(g.idle_event, 30000) != WAIT_OBJECT_0) Abort("allocator recycle timed out");
    }
    Must(g.allocators[slot]->Reset(), "allocator Reset");
    Must(g.list12->Reset(g.allocators[slot].Get(), nullptr), "list Reset");
    g.open_allocator = slot;
    return g.list12.Get();
}

uint64_t End12() {
    Must(g.list12->Close(), "list Close");
    ID3D12CommandList* lists[] = {g.list12.Get()};
    g.queue12->ExecuteCommandLists(1, lists);
    Must(g.queue12->Signal(g.idle12.Get(), ++g.idle_value), "idle Signal");
    g.allocator_retire[g.open_allocator] = g.idle_value;
    return g.idle_value;
}

// The idle fence's value once everything queued so far has run.
uint64_t Mark12() {
    Must(g.queue12->Signal(g.idle12.Get(), ++g.idle_value), "idle Signal");
    return g.idle_value;
}

void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                D3D12_RESOURCE_STATES after) {
    if (before == after) return;
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    list->ResourceBarrier(1, &barrier);
}

void UavBarrier(ID3D12GraphicsCommandList* list, ID3D12Resource* resource) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    barrier.UAV.pResource = resource;
    list->ResourceBarrier(1, &barrier);
}

ComPtr<ID3D12Resource> CreateBuffer12(D3D12_HEAP_TYPE type, UINT64 size, D3D12_RESOURCE_STATES state) {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = type;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> buffer;
    Must(g.device12->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                             IID_PPV_ARGS(&buffer)),
         "CreateCommittedResource(buffer)");
    Keep(buffer);
    return buffer;
}

ComPtr<ID3D12Resource> CreatePrivate12(uint32_t width, uint32_t height, DXGI_FORMAT format,
                                       D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state) {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Flags = flags;
    ComPtr<ID3D12Resource> texture;
    Must(g.device12->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                             IID_PPV_ARGS(&texture)),
         "CreateCommittedResource(private texture)");
    Keep(texture);
    return texture;
}

struct Readback12 {
    ComPtr<ID3D12Resource> buffer;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
    UINT rows = 0;
    UINT64 row_bytes = 0;
    UINT64 total = 0;
};

// Records a copy of one subresource (already in COPY_SOURCE) into a readback buffer.
Readback12 RecordReadback12(ID3D12GraphicsCommandList* list, ID3D12Resource* texture, UINT subresource) {
    const D3D12_RESOURCE_DESC desc = texture->GetDesc();
    Readback12 readback;
    g.device12->GetCopyableFootprints(&desc, subresource, 1, 0, &readback.layout, &readback.rows,
                                      &readback.row_bytes, &readback.total);
    readback.buffer = CreateBuffer12(D3D12_HEAP_TYPE_READBACK, readback.total, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = readback.buffer.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = readback.layout;
    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = texture;
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    source.SubresourceIndex = subresource;
    list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    return readback;
}

// Tightly packed rows, once the copy has completed.
std::vector<uint8_t> Unpack(const Readback12& readback) {
    std::vector<uint8_t> bytes(static_cast<std::size_t>(readback.row_bytes) * readback.rows);
    D3D12_RANGE range{0, static_cast<SIZE_T>(readback.total)};
    void* data = nullptr;
    Must(readback.buffer->Map(0, &range, &data), "readback Map");
    const auto* base = static_cast<const uint8_t*>(data) + readback.layout.Offset;
    for (UINT row = 0; row < readback.rows; ++row) {
        std::memcpy(bytes.data() + static_cast<std::size_t>(row) * readback.row_bytes,
                    base + static_cast<std::size_t>(row) * readback.layout.Footprint.RowPitch,
                    static_cast<std::size_t>(readback.row_bytes));
    }
    D3D12_RANGE written{0, 0};
    readback.buffer->Unmap(0, &written);
    return bytes;
}

// Records an upload of tightly packed rows into one subresource (already in COPY_DEST).
void RecordUpload12(ID3D12GraphicsCommandList* list, ID3D12Resource* texture, UINT subresource,
                    const std::vector<uint8_t>& bytes) {
    const D3D12_RESOURCE_DESC desc = texture->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout{};
    UINT rows = 0;
    UINT64 row_bytes = 0;
    UINT64 total = 0;
    g.device12->GetCopyableFootprints(&desc, subresource, 1, 0, &layout, &rows, &row_bytes, &total);
    ComPtr<ID3D12Resource> upload = CreateBuffer12(D3D12_HEAP_TYPE_UPLOAD, total, D3D12_RESOURCE_STATE_GENERIC_READ);
    void* data = nullptr;
    Must(upload->Map(0, nullptr, &data), "upload Map");
    auto* base = static_cast<uint8_t*>(data) + layout.Offset;
    for (UINT row = 0; row < rows; ++row) {
        std::memcpy(base + static_cast<std::size_t>(row) * layout.Footprint.RowPitch,
                    bytes.data() + static_cast<std::size_t>(row) * row_bytes, static_cast<std::size_t>(row_bytes));
    }
    upload->Unmap(0, nullptr);
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = texture;
    destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    destination.SubresourceIndex = subresource;
    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = upload.Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    source.PlacedFootprint = layout;
    list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
}

UINT AllocateSrv12(UINT count) {
    if (g.srv_next + count > g.srv_heap->GetDesc().NumDescriptors) Abort("SRV heap exhausted");
    const UINT index = g.srv_next;
    g.srv_next += count;
    return index;
}

D3D12_CPU_DESCRIPTOR_HANDLE SrvCpu(UINT index) {
    D3D12_CPU_DESCRIPTOR_HANDLE handle = g.srv_heap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(index) * g.srv_increment;
    return handle;
}

D3D12_GPU_DESCRIPTOR_HANDLE SrvGpu(UINT index) {
    D3D12_GPU_DESCRIPTOR_HANDLE handle = g.srv_heap->GetGPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<UINT64>(index) * g.srv_increment;
    return handle;
}

D3D12_CPU_DESCRIPTOR_HANDLE Rtv12(ID3D12Resource* texture, DXGI_FORMAT view) {
    if (g.rtv_next >= g.rtv_heap->GetDesc().NumDescriptors) Abort("RTV heap exhausted");
    D3D12_CPU_DESCRIPTOR_HANDLE handle = g.rtv_heap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += static_cast<SIZE_T>(g.rtv_next++) * g.rtv_increment;
    D3D12_RENDER_TARGET_VIEW_DESC desc{};
    desc.Format = view;
    desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    g.device12->CreateRenderTargetView(texture, &desc, handle);
    return handle;
}

// A descriptor table of [SRV t0, UAV u0] for the decode shader.
D3D12_GPU_DESCRIPTOR_HANDLE DecodeTable12(ID3D12Resource* source, DXGI_FORMAT view, ID3D12Resource* output) {
    const UINT index = AllocateSrv12(2);
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = view;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    g.device12->CreateShaderResourceView(source, &srv, SrvCpu(index));
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
    uav.Format = output->GetDesc().Format;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    g.device12->CreateUnorderedAccessView(output, nullptr, &uav, SrvCpu(index + 1));
    return SrvGpu(index);
}

struct DecodeConstants {
    uint32_t seed = 0;
    uint32_t mode = 0;
    uint32_t iterations = 0;
    uint32_t pad0 = 0;
    uint32_t offset_x = 0;
    uint32_t offset_y = 0;
    uint32_t pad1 = 0;
    uint32_t pad2 = 0;
};

// Loads `source` through its typed view at `offset`, into `output` (which sets the extent).
void RecordDecode12(ID3D12GraphicsCommandList* list, D3D12_GPU_DESCRIPTOR_HANDLE table, uint32_t width,
                    uint32_t height, uint32_t offset_x, uint32_t offset_y) {
    ID3D12DescriptorHeap* heaps[] = {g.srv_heap.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(g.root_signature.Get());
    list->SetPipelineState(g.decode_pso.Get());
    DecodeConstants constants;
    constants.offset_x = offset_x;
    constants.offset_y = offset_y;
    list->SetComputeRoot32BitConstants(0, 8, &constants, 0);
    list->SetComputeRootDescriptorTable(1, table);
    list->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
}

// --- Direct3D 11 plumbing --------------------------------------------------------------------------------------

void WaitIdle11() {
    D3D11_QUERY_DESC desc{D3D11_QUERY_EVENT, 0};
    ComPtr<ID3D11Query> query;
    Must(g.device11->CreateQuery(&desc, &query), "CreateQuery(event)");
    g.context11->End(query.Get());
    BOOL done = FALSE;
    const double start = NowMs();
    HRESULT hr = S_FALSE;
    while ((hr = g.context11->GetData(query.Get(), &done, sizeof(done), 0)) == S_FALSE) {
        if (NowMs() - start > 30000.0) Abort("D3D11 context did not go idle in 30 s");
        std::this_thread::yield();
    }
    if (hr != S_OK) Abort("D3D11 event query failed " + HrText(hr));
}

D3D11_TEXTURE2D_DESC Desc11(uint32_t width, uint32_t height, DXGI_FORMAT format, UINT bind, UINT array_size = 1,
                            UINT samples = 1, UINT misc = 0) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = array_size;
    desc.Format = format;
    desc.SampleDesc.Count = samples;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = bind;
    desc.MiscFlags = misc;
    return desc;
}

ComPtr<ID3D11Texture2D> CreateTexture11(const D3D11_TEXTURE2D_DESC& desc, const std::vector<uint8_t>* data = nullptr) {
    D3D11_SUBRESOURCE_DATA initial{};
    if (data) {
        initial.pSysMem = data->data();
        initial.SysMemPitch = desc.Width * BytesPerPixel(desc.Format);
    }
    ComPtr<ID3D11Texture2D> texture;
    Must(g.device11->CreateTexture2D(&desc, data ? &initial : nullptr, &texture), "CreateTexture2D(private)");
    return texture;
}

ComPtr<ID3D11Texture2D> CreateTexture11(const D3D11_TEXTURE2D_DESC& desc, const std::vector<uint8_t>& data) {
    return CreateTexture11(desc, &data);
}

HRESULT CreateRtv11(ID3D11Texture2D* texture, DXGI_FORMAT view, ComPtr<ID3D11RenderTargetView>& rtv) {
    D3D11_TEXTURE2D_DESC texture_desc{};
    texture->GetDesc(&texture_desc);
    D3D11_RENDER_TARGET_VIEW_DESC desc{};
    desc.Format = view;
    if (texture_desc.SampleDesc.Count > 1) {
        desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMS;
    } else if (texture_desc.ArraySize > 1) {
        desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
        desc.Texture2DArray.ArraySize = texture_desc.ArraySize;
    } else {
        desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    }
    return g.device11->CreateRenderTargetView(texture, &desc, &rtv);
}

HRESULT CreateSrv11(ID3D11Texture2D* texture, DXGI_FORMAT view, ComPtr<ID3D11ShaderResourceView>& srv) {
    D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
    desc.Format = view;
    desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    desc.Texture2D.MipLevels = 1;
    return g.device11->CreateShaderResourceView(texture, &desc, &srv);
}

HRESULT CreateUav11(ID3D11Texture2D* texture, DXGI_FORMAT view, ComPtr<ID3D11UnorderedAccessView>& uav) {
    D3D11_UNORDERED_ACCESS_VIEW_DESC desc{};
    desc.Format = view;
    desc.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
    return g.device11->CreateUnorderedAccessView(texture, &desc, &uav);
}

// Direct3D 11 holds key 0 around its own use of a keyed-mutex texture; Direct3D 12 never takes the mutex.
// AcquireSync returns WAIT_TIMEOUT and WAIT_ABANDONED as success codes, so only S_OK counts.
class Access11 {
public:
    explicit Access11(IDXGIKeyedMutex* mutex) : mutex_(mutex) {
        if (mutex_) hr_ = mutex_->AcquireSync(0, kMutexTimeoutMs);
    }
    ~Access11() { Release(); }
    void Release() {
        if (mutex_ && hr_ == S_OK) {
            const HRESULT released = mutex_->ReleaseSync(0);
            if (released != S_OK) Abort("ReleaseSync " + HrText(released));
        }
        mutex_ = nullptr;
    }
    Access11(const Access11&) = delete;
    Access11& operator=(const Access11&) = delete;
    bool ok() const { return hr_ == S_OK; }
    HRESULT result() const { return hr_; }

private:
    IDXGIKeyedMutex* mutex_ = nullptr;
    HRESULT hr_ = S_OK;
};

// Tightly packed bytes of one subresource of a single-sample texture.
std::vector<uint8_t> Read11(ID3D11Texture2D* texture, UINT subresource, IDXGIKeyedMutex* mutex = nullptr) {
    D3D11_TEXTURE2D_DESC desc{};
    texture->GetDesc(&desc);
    D3D11_TEXTURE2D_DESC staging_desc = Desc11(desc.Width, desc.Height, desc.Format, 0);
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    Must(g.device11->CreateTexture2D(&staging_desc, nullptr, &staging), "CreateTexture2D(staging)");
    {
        Access11 access(mutex);
        if (!access.ok()) Abort("AcquireSync for readback failed " + HrText(access.result()));
        g.context11->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, texture, subresource, nullptr);
    }
    D3D11_MAPPED_SUBRESOURCE mapped{};
    Must(g.context11->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Map(staging)");
    const std::size_t row_bytes = static_cast<std::size_t>(desc.Width) * BytesPerPixel(desc.Format);
    std::vector<uint8_t> bytes(row_bytes * desc.Height);
    for (UINT row = 0; row < desc.Height; ++row) {
        std::memcpy(bytes.data() + row * row_bytes, static_cast<const uint8_t*>(mapped.pData) + row * mapped.RowPitch,
                    row_bytes);
    }
    g.context11->Unmap(staging.Get(), 0);
    return bytes;
}

struct Constants11 {
    uint32_t seed = 0;
    uint32_t mode = 0;
    uint32_t iterations = 0;
    uint32_t pad0 = 0;
    uint32_t offset_x = 0;
    uint32_t offset_y = 0;
    uint32_t pad1 = 0;
    uint32_t pad2 = 0;
};

void SetConstants11(const Constants11& constants) {
    g.context11->UpdateSubresource(g.constants11.Get(), 0, nullptr, &constants, 0, 0);
}

enum class PatternMode : uint32_t { Noise = 0, Smooth = 1 };

// One full-screen triangle through `ps` into `rtv` (and/or `dsv`), clipped to the viewport.
void Draw11(ID3D11PixelShader* ps, ID3D11RenderTargetView* rtv, ID3D11DepthStencilView* dsv, float x, float y,
            float width, float height, const Constants11& constants, bool blend = false,
            ID3D11ShaderResourceView* source = nullptr) {
    SetConstants11(constants);
    ID3D11DeviceContext4* c = g.context11.Get();
    c->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    c->IASetInputLayout(nullptr);
    c->VSSetShader(g.fullscreen_vs.Get(), nullptr, 0);
    c->PSSetShader(ps, nullptr, 0);
    ID3D11Buffer* buffers[] = {g.constants11.Get()};
    c->PSSetConstantBuffers(0, 1, buffers);
    c->PSSetShaderResources(0, 1, &source);
    c->RSSetState(g.rasterizer.Get());
    const D3D11_VIEWPORT viewport{x, y, width, height, 0.0f, 1.0f};
    c->RSSetViewports(1, &viewport);
    c->OMSetRenderTargets(rtv ? 1 : 0, rtv ? &rtv : nullptr, dsv);
    c->OMSetBlendState(blend ? g.blend_alpha.Get() : nullptr, nullptr, 0xFFFFFFFF);
    c->OMSetDepthStencilState(dsv ? g.depth_always.Get() : nullptr, 0);
    c->Draw(3, 0);
    ID3D11ShaderResourceView* null_srv = nullptr;
    c->PSSetShaderResources(0, 1, &null_srv);
    c->OMSetRenderTargets(0, nullptr, nullptr);
}

void Dispatch11(ID3D11ComputeShader* cs, ID3D11ShaderResourceView* source, ID3D11UnorderedAccessView* output,
                uint32_t width, uint32_t height, uint32_t offset_x = 0, uint32_t offset_y = 0) {
    Constants11 constants;
    constants.offset_x = offset_x;
    constants.offset_y = offset_y;
    SetConstants11(constants);
    ID3D11DeviceContext4* c = g.context11.Get();
    c->CSSetShader(cs, nullptr, 0);
    ID3D11Buffer* buffers[] = {g.constants11.Get()};
    c->CSSetConstantBuffers(0, 1, buffers);
    c->CSSetShaderResources(0, 1, &source);
    c->CSSetUnorderedAccessViews(0, 1, &output, nullptr);
    c->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
    ID3D11ShaderResourceView* null_srv = nullptr;
    ID3D11UnorderedAccessView* null_uav = nullptr;
    c->CSSetShaderResources(0, 1, &null_srv);
    c->CSSetUnorderedAccessViews(0, 1, &null_uav, nullptr);
}

constexpr float kClearA[4] = {0.25f, 0.5f, 0.75f, 1.0f};
constexpr float kClearB[4] = {0.9f, 0.1f, 0.3f, 0.6f};

// A fast clear, a noise draw over the middle and a partial clear: the three ways a game writes a target.
void RenderPattern11(ID3D11RenderTargetView* rtv, uint32_t width, uint32_t height, uint32_t seed) {
    g.context11->ClearRenderTargetView(rtv, kClearA);
    Constants11 constants;
    constants.seed = seed;
    constants.mode = static_cast<uint32_t>(PatternMode::Noise);
    Draw11(g.pattern_ps.Get(), rtv, nullptr, static_cast<float>(width / 8), static_cast<float>(height / 8),
           static_cast<float>(width - 2 * (width / 8)), static_cast<float>(height - 2 * (height / 8)), constants);
    const D3D11_RECT corner{0, 0, static_cast<LONG>(std::max(1u, width / 4)), static_cast<LONG>(std::max(1u, height / 4))};
    g.context11->ClearView(rtv, kClearB, &corner, 1);
}

// Noise bytes; half floats are kept finite so no copy path has a reason to touch them.
std::vector<uint8_t> PatternBytes(std::size_t count, uint32_t seed, DXGI_FORMAT format) {
    std::vector<uint8_t> bytes(count);
    uint32_t state = seed * 0x9E3779B9u + 0x7F4A7C15u;
    for (std::size_t i = 0; i < count; i += 4) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        std::memcpy(bytes.data() + i, &state, std::min<std::size_t>(4, count - i));
    }
    if (IsHalfFloat(format)) {
        for (std::size_t i = 0; i + 1 < count; i += 2) {
            uint16_t half = 0;
            std::memcpy(&half, bytes.data() + i, 2);
            if ((half & 0x7C00) == 0x7C00) half &= 0xFBFF;
            std::memcpy(bytes.data() + i, &half, 2);
        }
    }
    return bytes;
}

struct Comparison {
    std::size_t mismatches = 0;
    std::size_t first = 0;
    bool size_mismatch = false;
};

Comparison CompareBytes(const std::vector<uint8_t>& expected, const std::vector<uint8_t>& actual) {
    Comparison result;
    if (expected.size() != actual.size()) {
        result.size_mismatch = true;
        result.mismatches = std::max(expected.size(), actual.size());
        return result;
    }
    if (std::memcmp(expected.data(), actual.data(), expected.size()) == 0) return result;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        if (expected[i] == actual[i]) continue;
        if (result.mismatches++ == 0) result.first = i;
    }
    return result;
}

// CompareBytes over pixels of `format`, ignoring bytes the format leaves undefined.
Comparison ComparePixels(const std::vector<uint8_t>& expected, const std::vector<uint8_t>& actual,
                         DXGI_FORMAT format) {
    if (!HasUndefinedAlpha(format) || expected.size() != actual.size()) return CompareBytes(expected, actual);
    std::vector<uint8_t> a = expected;
    std::vector<uint8_t> b = actual;
    for (std::size_t i = 3; i < a.size(); i += 4) a[i] = b[i] = 0;
    return CompareBytes(a, b);
}

std::string DescribeMismatch(const Comparison& comparison, uint32_t width, uint32_t bytes_per_pixel) {
    if (comparison.size_mismatch) return "size mismatch";
    const std::size_t pixel = comparison.first / bytes_per_pixel;
    return std::to_string(comparison.mismatches) + " bytes differ, first at pixel (" +
           std::to_string(pixel % width) + "," + std::to_string(pixel / width) + ") byte " +
           std::to_string(comparison.first % bytes_per_pixel);
}

// Distinct 32-bit words in a sample: proof a reference is not blank, so a match means something.
std::size_t Variety(const std::vector<uint8_t>& bytes) {
    std::set<uint32_t> seen;
    const std::size_t words = bytes.size() / 4;
    const std::size_t stride = std::max<std::size_t>(1, words / 4096);
    for (std::size_t i = 0; i < words && seen.size() < 4096; i += stride) {
        uint32_t word = 0;
        std::memcpy(&word, bytes.data() + i * 4, 4);
        seen.insert(word);
    }
    return seen.size();
}

// --- Shared textures -------------------------------------------------------------------------------------------

enum class Lineage { D3D11KeyedMutex, D3D11Shared, D3D12Simultaneous, D3D12Plain };
constexpr std::array<Lineage, 4> kLineages{Lineage::D3D11KeyedMutex, Lineage::D3D11Shared,
                                           Lineage::D3D12Simultaneous, Lineage::D3D12Plain};

const char* LineageName(Lineage lineage) {
    switch (lineage) {
    case Lineage::D3D11KeyedMutex: return "A_keyed_mutex";
    case Lineage::D3D11Shared: return "A_nt_shared";
    case Lineage::D3D12Simultaneous: return "B_simultaneous";
    case Lineage::D3D12Plain: return "B_plain";
    }
    return "?";
}

struct TextureSpec {
    uint32_t width = 0;
    uint32_t height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    uint32_t array_size = 1;
    uint32_t samples = 1;
    bool render_target = true;
    bool unordered_access = false;
    bool depth_stencil = false;
    bool shader_resource = true;
};

struct SharedTexture {
    ComPtr<ID3D11Texture2D> texture11;
    ComPtr<ID3D12Resource> texture12;
    ComPtr<IDXGIKeyedMutex> mutex;
    std::string stage;
    HRESULT hr = S_OK;
    bool ok() const { return texture11 && texture12; }
};

UINT BindFlags11(const TextureSpec& spec) {
    if (spec.depth_stencil) {
        return D3D11_BIND_DEPTH_STENCIL | (spec.shader_resource ? D3D11_BIND_SHADER_RESOURCE : 0);
    }
    UINT bind = D3D11_BIND_SHADER_RESOURCE;
    if (spec.render_target) bind |= D3D11_BIND_RENDER_TARGET;
    if (spec.unordered_access) bind |= D3D11_BIND_UNORDERED_ACCESS;
    return bind;
}

D3D12_RESOURCE_FLAGS Flags12(const TextureSpec& spec) {
    if (spec.depth_stencil) return D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE;
    if (spec.render_target) flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if (spec.unordered_access) flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    return flags;
}

// Creates a texture on one API with exactly these flags and opens it on the other. Direct3D 11 owns it when
// `misc11` is non-zero; otherwise Direct3D 12 creates it on a shared heap with `flags12`.
SharedTexture CreateSharedWith(UINT misc11, UINT bind11, D3D12_RESOURCE_FLAGS flags12, const TextureSpec& spec,
                               const D3D12_CLEAR_VALUE* clear = nullptr) {
    SharedTexture shared;
    HANDLE handle = nullptr;
    if (misc11 != 0) {
        const UINT misc = misc11;
        const D3D11_TEXTURE2D_DESC desc =
            Desc11(spec.width, spec.height, spec.format, bind11, spec.array_size, spec.samples, misc);
        shared.hr = g.device11->CreateTexture2D(&desc, nullptr, &shared.texture11);
        if (FAILED(shared.hr)) {
            shared.stage = "d3d11_create";
            return shared;
        }
        ComPtr<IDXGIResource1> resource;
        shared.hr = shared.texture11.As(&resource);
        if (SUCCEEDED(shared.hr)) {
            shared.hr = resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                                                     nullptr, &handle);
        }
        if (FAILED(shared.hr)) {
            shared.stage = "d3d11_create_shared_handle";
            shared.texture11.Reset();
            return shared;
        }
        shared.hr = g.device12->OpenSharedHandle(handle, IID_PPV_ARGS(&shared.texture12));
        CloseHandle(handle);
        if (FAILED(shared.hr)) {
            shared.stage = "d3d12_open";
            shared.texture11.Reset();
            return shared;
        }
        if ((misc & D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX) != 0) {
            shared.hr = shared.texture11.As(&shared.mutex);
            if (FAILED(shared.hr)) {
                shared.stage = "query_keyed_mutex";
                shared.texture11.Reset();
                shared.texture12.Reset();
                return shared;
            }
        }
    } else {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = spec.width;
        desc.Height = spec.height;
        desc.DepthOrArraySize = static_cast<UINT16>(spec.array_size);
        desc.MipLevels = 1;
        desc.Format = spec.format;
        desc.SampleDesc.Count = spec.samples;
        desc.Flags = flags12;
        shared.hr = g.device12->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &desc,
                                                        D3D12_RESOURCE_STATE_COMMON, clear,
                                                        IID_PPV_ARGS(&shared.texture12));
        if (FAILED(shared.hr)) {
            shared.stage = "d3d12_create";
            return shared;
        }
        shared.hr = g.device12->CreateSharedHandle(shared.texture12.Get(), nullptr, GENERIC_ALL, nullptr, &handle);
        if (FAILED(shared.hr)) {
            shared.stage = "d3d12_create_shared_handle";
            shared.texture12.Reset();
            return shared;
        }
        shared.hr = g.device11->OpenSharedResource1(handle, IID_PPV_ARGS(&shared.texture11));
        CloseHandle(handle);
        if (FAILED(shared.hr)) {
            shared.stage = "d3d11_open";
            shared.texture12.Reset();
            return shared;
        }
    }
    Keep(shared.texture12);
    return shared;
}

SharedTexture CreateShared(Lineage lineage, const TextureSpec& spec) {
    switch (lineage) {
    case Lineage::D3D11KeyedMutex:
        return CreateSharedWith(D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX,
                                BindFlags11(spec), D3D12_RESOURCE_FLAG_NONE, spec);
    case Lineage::D3D11Shared:
        return CreateSharedWith(D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED, BindFlags11(spec),
                                D3D12_RESOURCE_FLAG_NONE, spec);
    case Lineage::D3D12Simultaneous:
        return CreateSharedWith(0, 0, Flags12(spec) | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS, spec);
    case Lineage::D3D12Plain:
        return CreateSharedWith(0, 0, Flags12(spec), spec);
    }
    return {};
}

// What each API reports for a shared texture: Direct3D 11's bind and misc flags as opened or created, and
// Direct3D 12's resource flags and heap flags.
void DescribeShared(const SharedTexture& shared, Record& record) {
    D3D11_TEXTURE2D_DESC desc11{};
    shared.texture11->GetDesc(&desc11);
    const D3D12_RESOURCE_DESC desc12 = shared.texture12->GetDesc();
    D3D12_HEAP_PROPERTIES heap{};
    D3D12_HEAP_FLAGS heap_flags = D3D12_HEAP_FLAG_NONE;
    const HRESULT heap_hr = shared.texture12->GetHeapProperties(&heap, &heap_flags);
    record.detail = "d3d11 bind=" + Hex(desc11.BindFlags, 3) + " misc=" + Hex(desc11.MiscFlags, 4) +
                    "; d3d12 flags=" + Hex(desc12.Flags, 3) + " layout=" + std::to_string(desc12.Layout) +
                    (SUCCEEDED(heap_hr) ? " heap_flags=" + Hex(heap_flags, 4) : " heap_flags=? " + HrText(heap_hr));
}

void Settle() {
    WaitIdle12();
    WaitIdle11();
    g.graveyard.clear();
    g.srv_next = 0;
    g.rtv_next = 0;
    DrainStray();
    const HRESULT removed11 = g.device11->GetDeviceRemovedReason();
    const HRESULT removed12 = g.device12->GetDeviceRemovedReason();
    if (FAILED(removed11) || FAILED(removed12)) {
        Record record{"devices", "device_removed", "fail"};
        record.detail = "d3d11 " + HrText(removed11) + ", d3d12 " + HrText(removed12);
        Finish(record);
        Abort("a device was removed");
    }
}

// D3D11 signals the next "written" value and flushes so the signal is submitted; D3D12 queues a wait for it.
uint64_t Publish11To12(bool queue_wait = true) {
    const uint64_t value = ++g_to12.value;
    Must(g.context11->Signal(g_to12.fence11.Get(), value), "ID3D11DeviceContext4::Signal");
    g.context11->Flush();
    if (queue_wait) Must(g.queue12->Wait(g_to12.fence12.Get(), value), "ID3D12CommandQueue::Wait");
    return value;
}

// D3D12 signals the next "done" value; D3D11 queues a wait for it unless the caller waits later.
uint64_t Publish12To11(bool context_wait = true) {
    const uint64_t value = ++g_to11.value;
    Must(g.queue12->Signal(g_to11.fence12.Get(), value), "ID3D12CommandQueue::Signal");
    if (context_wait) Must(g.context11->Wait(g_to11.fence11.Get(), value), "ID3D11DeviceContext4::Wait");
    return value;
}

HRESULT CreateSharedFence(SharedFence& fence) {
    HRESULT hr = g.device12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence.fence12));
    if (FAILED(hr)) return hr;
    HANDLE handle = nullptr;
    hr = g.device12->CreateSharedHandle(fence.fence12.Get(), nullptr, GENERIC_ALL, nullptr, &handle);
    if (FAILED(hr)) return hr;
    hr = g.device11->OpenSharedFence(handle, IID_PPV_ARGS(&fence.fence11));
    CloseHandle(handle);
    return hr;
}

// --- Shaders ---------------------------------------------------------------------------------------------------

constexpr char kCommonHlsl[] = R"(
cbuffer Constants : register(b0) {
    uint seed;
    uint mode;
    uint iterations;
    uint pad0;
    uint2 offset;
    uint2 pad1;
};
uint Hash(uint x) {
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
    return x;
}
)";

constexpr char kGraphicsHlsl[] = R"(
float4 FullscreenVs(uint id : SV_VertexID) : SV_Position {
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}
// mode 0: noise, every pixel and channel different; mode 1: a smooth gradient that compresses well.
float4 PatternPs(float4 position : SV_Position) : SV_Target {
    uint2 p = uint2(position.xy);
    if (mode == 1) return float4(frac(p.x / 1024.0 + seed * 0.0625), frac(p.y / 1024.0), 0.5, 1.0);
    uint h = Hash(p.x ^ Hash(p.y ^ Hash(seed)));
    for (uint i = 0; i < iterations; ++i) h = Hash(h + i);
    uint k = Hash(h);
    return float4(h & 0xFFFF, h >> 16, k & 0xFFFF, k >> 16) / 65535.0;
}
float DepthPs(float4 position : SV_Position) : SV_Depth {
    uint2 p = uint2(position.xy);
    if (mode == 1) return frac((p.x + p.y) / 4096.0);
    return (Hash(p.x ^ Hash(p.y ^ Hash(seed))) & 0xFFFFFF) / 16777215.0;
}
Texture2D<float4> Source : register(t0);
float4 ReadPs(float4 position : SV_Position) : SV_Target {
    return Source.Load(int3(position.xy, 0)) * 0.5;
}
)";

constexpr char kDecodeHlsl[] = R"(
Texture2D<float4> Source : register(t0);
RWTexture2D<float4> Output : register(u0);
[numthreads(8, 8, 1)]
void DecodeCs(uint3 id : SV_DispatchThreadID) {
    uint w, h;
    Output.GetDimensions(w, h);
    if (id.x < w && id.y < h) Output[id.xy] = Source.Load(int3(id.xy + offset, 0));
}
// Reads a 4x4 block per thread and writes one value: a pass whose cost is the source read.
[numthreads(8, 8, 1)]
void ReduceCs(uint3 id : SV_DispatchThreadID) {
    uint w, h;
    Output.GetDimensions(w, h);
    if (id.x >= w || id.y >= h) return;
    float4 sum = 0;
    [unroll] for (uint y = 0; y < 4; ++y) {
        [unroll] for (uint x = 0; x < 4; ++x) sum += Source.Load(int3(id.xy * 4 + uint2(x, y), 0));
    }
    Output[id.xy] = sum * (1.0 / 16.0);
}
)";

constexpr char kDepthCopyHlsl[] = R"(
Texture2D<float> Source : register(t0);
RWTexture2D<float> Output : register(u0);
[numthreads(8, 8, 1)]
void DepthCopyCs(uint3 id : SV_DispatchThreadID) {
    uint w, h;
    Output.GetDimensions(w, h);
    if (id.x < w && id.y < h) Output[id.xy] = Source.Load(int3(id.xy, 0));
}
)";

ComPtr<ID3DBlob> Compile(const std::string& source, const char* entry, const char* target) {
    ComPtr<ID3DBlob> code;
    ComPtr<ID3DBlob> errors;
    const HRESULT hr = D3DCompile(source.data(), source.size(), entry, nullptr, nullptr, entry, target,
                                  D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (FAILED(hr)) {
        Abort(std::string("compiling ") + entry + ": " +
              (errors ? static_cast<const char*>(errors->GetBufferPointer()) : HrText(hr)));
    }
    return code;
}

void CreatePipelines() {
    const std::string graphics = std::string(kCommonHlsl) + kGraphicsHlsl;
    const std::string decode = std::string(kCommonHlsl) + kDecodeHlsl;
    const std::string depth_copy = std::string(kCommonHlsl) + kDepthCopyHlsl;
    const auto vs = Compile(graphics, "FullscreenVs", "vs_5_0");
    const auto pattern = Compile(graphics, "PatternPs", "ps_5_0");
    const auto depth = Compile(graphics, "DepthPs", "ps_5_0");
    const auto read = Compile(graphics, "ReadPs", "ps_5_0");
    const auto decode_cs = Compile(decode, "DecodeCs", "cs_5_0");
    const auto reduce_cs = Compile(decode, "ReduceCs", "cs_5_0");
    g.fullscreen_vs_code = vs;
    g.pattern_ps_code = pattern;
    const auto depth_cs = Compile(depth_copy, "DepthCopyCs", "cs_5_0");

    ID3D11Device5* d = g.device11.Get();
    Must(d->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr, &g.fullscreen_vs), "VS");
    Must(d->CreatePixelShader(pattern->GetBufferPointer(), pattern->GetBufferSize(), nullptr, &g.pattern_ps), "PS");
    Must(d->CreatePixelShader(depth->GetBufferPointer(), depth->GetBufferSize(), nullptr, &g.depth_ps), "PS");
    Must(d->CreatePixelShader(read->GetBufferPointer(), read->GetBufferSize(), nullptr, &g.read_ps), "PS");
    Must(d->CreateComputeShader(decode_cs->GetBufferPointer(), decode_cs->GetBufferSize(), nullptr, &g.decode_cs),
         "CS");
    Must(d->CreateComputeShader(reduce_cs->GetBufferPointer(), reduce_cs->GetBufferSize(), nullptr, &g.reduce_cs),
         "CS");
    Must(d->CreateComputeShader(depth_cs->GetBufferPointer(), depth_cs->GetBufferSize(), nullptr,
                                &g.depth_copy_cs),
         "CS");
    D3D11_BUFFER_DESC cb{};
    cb.ByteWidth = sizeof(Constants11);
    cb.Usage = D3D11_USAGE_DEFAULT;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    Must(d->CreateBuffer(&cb, nullptr, &g.constants11), "constant buffer");
    D3D11_RASTERIZER_DESC rasterizer{};
    rasterizer.FillMode = D3D11_FILL_SOLID;
    rasterizer.CullMode = D3D11_CULL_NONE;
    rasterizer.DepthClipEnable = TRUE;
    Must(d->CreateRasterizerState(&rasterizer, &g.rasterizer), "rasterizer");
    D3D11_DEPTH_STENCIL_DESC depth_state{};
    depth_state.DepthEnable = TRUE;
    depth_state.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    depth_state.DepthFunc = D3D11_COMPARISON_ALWAYS;
    Must(d->CreateDepthStencilState(&depth_state, &g.depth_always), "depth state");
    D3D11_BLEND_DESC blend{};
    blend.RenderTarget[0].BlendEnable = TRUE;
    blend.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blend.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blend.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blend.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    blend.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blend.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    Must(d->CreateBlendState(&blend, &g.blend_alpha), "blend state");

    // Direct3D 12: root constants at b0 and a [SRV t0, UAV u0] table.
    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 1;
    ranges[0].OffsetInDescriptorsFromTableStart = 0;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 1;
    ranges[1].OffsetInDescriptorsFromTableStart = 1;
    D3D12_ROOT_PARAMETER parameters[2]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants.Num32BitValues = 8;
    parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable.NumDescriptorRanges = 2;
    parameters[1].DescriptorTable.pDescriptorRanges = ranges;
    parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC root{};
    root.NumParameters = 2;
    root.pParameters = parameters;
    ComPtr<ID3DBlob> serialized;
    ComPtr<ID3DBlob> errors;
    Must(D3D12SerializeRootSignature(&root, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors),
         "D3D12SerializeRootSignature");
    Must(g.device12->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                         IID_PPV_ARGS(&g.root_signature)),
         "CreateRootSignature");
    D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature = g.root_signature.Get();
    pso.CS = {decode_cs->GetBufferPointer(), decode_cs->GetBufferSize()};
    Must(g.device12->CreateComputePipelineState(&pso, IID_PPV_ARGS(&g.decode_pso)), "CreateComputePipelineState");
    pso.CS = {reduce_cs->GetBufferPointer(), reduce_cs->GetBufferSize()};
    Must(g.device12->CreateComputePipelineState(&pso, IID_PPV_ARGS(&g.reduce_pso)), "CreateComputePipelineState");
}

// --- Devices, fences and capabilities --------------------------------------------------------------------------

void CreateDevices() {
    Record record{"devices", "same_adapter"};
    ComPtr<IDXGIFactory6> factory;
    if (!g_options.debug || FAILED(CreateDXGIFactory2(DXGI_CREATE_FACTORY_DEBUG, IID_PPV_ARGS(&factory)))) {
        Must(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "CreateDXGIFactory2");
    }
    if (g_options.debug) {
        ComPtr<ID3D12Debug> debug;
        if (FAILED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) {
            Abort("the D3D12 debug layer (Graphics Tools) is not installed");
        }
        debug->EnableDebugLayer();
        // By default the debug layer holds D3D12 submissions on the CPU until every queued Wait is satisfied,
        // which would make the cross-API fence cases test the debug layer instead of the GPU. GPU-based
        // validation requires that mode, so --gpu-validation keeps it and skips the fence and ring cases.
        ComPtr<ID3D12Debug1> debug1;
        if (SUCCEEDED(debug.As(&debug1))) {
            debug1->SetEnableSynchronizedCommandQueueValidation(g_options.gpu_validation ? TRUE : FALSE);
            if (g_options.gpu_validation) debug1->SetEnableGPUBasedValidation(TRUE);
        }
        DXGIGetDebugInterface1(0, IID_PPV_ARGS(&g.info_dxgi));
    }
    // The game's adapter: the first high-performance hardware adapter.
    for (UINT index = 0;; ++index) {
        ComPtr<IDXGIAdapter1> candidate;
        if (factory->EnumAdapterByGpuPreference(index, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                                                IID_PPV_ARGS(&candidate)) == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        DXGI_ADAPTER_DESC1 desc{};
        if (FAILED(candidate->GetDesc1(&desc)) || (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) continue;
        g.adapter = candidate;
        g.adapter_desc = desc;
        break;
    }
    if (!g.adapter) Abort("no hardware adapter");
    g.adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &g.driver_version);

    // A Direct3D 11 device as a game creates one.
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    const D3D_FEATURE_LEVEL* first_level = g_options.feature_level_11_0 ? &levels[1] : &levels[0];
    const UINT level_count = g_options.feature_level_11_0 ? 1 : 2;
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    if (g_options.debug) flags |= D3D11_CREATE_DEVICE_DEBUG;
    if (g_options.single_threaded) flags |= D3D11_CREATE_DEVICE_SINGLETHREADED;
    ComPtr<ID3D11Device> device11;
    ComPtr<ID3D11DeviceContext> context11;
    Must(D3D11CreateDevice(g.adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, first_level, level_count,
                           D3D11_SDK_VERSION, &device11, &g.level11, &context11),
         "D3D11CreateDevice");
    Must(device11.As(&g.device11), "ID3D11Device5");
    Must(context11.As(&g.context11), "ID3D11DeviceContext4");

    // The bridge's way: Direct3D 12 on the D3D11 device's own adapter, not a fresh enumeration.
    ComPtr<IDXGIDevice> dxgi_device;
    Must(device11.As(&dxgi_device), "IDXGIDevice");
    ComPtr<IDXGIAdapter> adapter11;
    Must(dxgi_device->GetAdapter(&adapter11), "IDXGIDevice::GetAdapter");
    DXGI_ADAPTER_DESC desc11{};
    Must(adapter11->GetDesc(&desc11), "adapter GetDesc");
    Must(D3D12CreateDevice(adapter11.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g.device12)), "D3D12CreateDevice");
    const LUID luid12 = g.device12->GetAdapterLuid();
    const bool same = luid12.HighPart == desc11.AdapterLuid.HighPart && luid12.LowPart == desc11.AdapterLuid.LowPart;

    if (g_options.debug) {
        g.device11.As(&g.info11);
        g.device12.As(&g.info12);
        if (!g.info11 || !g.info12) Abort("debug info queues unavailable");
        // A performance note about optimized clear values, not about sharing.
        D3D12_MESSAGE_ID deny[] = {D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE};
        D3D12_INFO_QUEUE_FILTER filter{};
        filter.DenyList.NumIDs = static_cast<UINT>(std::size(deny));
        filter.DenyList.pIDList = deny;
        g.info12->AddStorageFilterEntries(&filter);
    }

    D3D12_COMMAND_QUEUE_DESC queue{};
    queue.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    Must(g.device12->CreateCommandQueue(&queue, IID_PPV_ARGS(&g.queue12)), "CreateCommandQueue");
    for (auto& allocator : g.allocators) {
        Must(g.device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)),
             "CreateCommandAllocator");
    }
    Must(g.device12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g.allocators[0].Get(), nullptr,
                                       IID_PPV_ARGS(&g.list12)),
         "CreateCommandList");
    Must(g.list12->Close(), "list Close");
    Must(g.device12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g.idle12)), "CreateFence(idle)");
    g.idle_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    D3D12_DESCRIPTOR_HEAP_DESC srv_heap{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1024,
                                        D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    Must(g.device12->CreateDescriptorHeap(&srv_heap, IID_PPV_ARGS(&g.srv_heap)), "SRV heap");
    g.srv_increment = g.device12->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_DESCRIPTOR_HEAP_DESC rtv_heap{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 64, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    Must(g.device12->CreateDescriptorHeap(&rtv_heap, IID_PPV_ARGS(&g.rtv_heap)), "RTV heap");
    g.rtv_increment = g.device12->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    D3D12_FEATURE_DATA_FEATURE_LEVELS levels12{};
    const D3D_FEATURE_LEVEL requested12[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_12_0,
                                             D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_2};
    levels12.NumFeatureLevels = static_cast<UINT>(std::size(requested12));
    levels12.pFeatureLevelsRequested = requested12;
    g.device12->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &levels12, sizeof(levels12));

    char adapter_name[256]{};
    WideCharToMultiByte(CP_UTF8, 0, g.adapter_desc.Description, -1, adapter_name, sizeof(adapter_name), nullptr,
                        nullptr);
    const auto part = [](LONG value, int shift) { return std::to_string((static_cast<uint32_t>(value) >> shift) & 0xFFFF); };
    const std::string driver = part(g.driver_version.HighPart, 16) + "." + part(g.driver_version.HighPart, 0) + "." +
                               part(g.driver_version.LowPart, 16) + "." + part(g.driver_version.LowPart, 0);
    record.status = same ? "pass" : "fail";
    record.detail = std::string(adapter_name) + ", driver " + driver + ", vendor " + Hex(g.adapter_desc.VendorId, 4) +
                    ", luid " + Hex(static_cast<uint32_t>(luid12.HighPart)) + ":" + Hex(luid12.LowPart) +
                    (same ? " (D3D11 and D3D12 match)" : " (MISMATCH)") + ", d3d11 level " +
                    Hex(g.level11, 4) + ", d3d12 max level " + Hex(levels12.MaxSupportedFeatureLevel, 4) +
                    ", d3d11 creation flags " + Hex(g.device11->GetCreationFlags(), 4) +
                    (g_options.single_threaded ? " (SINGLETHREADED)" : "") +
                    (g_options.debug ? std::string(", debug layers on (D3D11, D3D12") +
                                           (g.info_dxgi ? ", DXGI" : ", DXGI unavailable") +
                                           (g_options.gpu_validation
                                                ? "), D3D12 synchronized queue validation on (GBV needs it)"
                                                : "), D3D12 synchronized queue validation off")
                                     : "") +
                    (g_options.gpu_validation ? " + GBV" : "");
    Finish(record);
    if (!same) Abort("D3D11 and D3D12 devices are on different adapters");
}

void CreateFences() {
    Record record{"fences", "create_d3d12_shared_open_d3d11"};
    const HRESULT a = CreateSharedFence(g_to12);
    const HRESULT b = CreateSharedFence(g_to11);
    const HRESULT c = CreateSharedFence(g_gate);
    const bool created = SUCCEEDED(a) && SUCCEEDED(b) && SUCCEEDED(c);
    record.status = created ? "pass" : "fail";
    record.detail = "to12 " + HrText(a) + ", to11 " + HrText(b) + ", gate " + HrText(c);
    Finish(record);
    if (!created) Abort("shared fences are the foundation of every case");
}

void Capabilities() {
    {
        Record record{"capabilities", "sharing_tiers", "info"};
        D3D11_FEATURE_DATA_D3D11_OPTIONS options{};
        g.device11->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS, &options, sizeof(options));
        D3D11_FEATURE_DATA_D3D11_OPTIONS5 options5{};
        const HRESULT hr5 = g.device11->CheckFeatureSupport(D3D11_FEATURE_D3D11_OPTIONS5, &options5, sizeof(options5));
        D3D12_FEATURE_DATA_D3D12_OPTIONS4 options4{};
        const HRESULT hr4 = g.device12->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS4, &options4, sizeof(options4));
        D3D12_FEATURE_DATA_D3D12_OPTIONS options12{};
        g.device12->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options12, sizeof(options12));
        record.metrics = {{"d3d11_extended_resource_sharing", options.ExtendedResourceSharing},
                          {"d3d11_shared_resource_tier", SUCCEEDED(hr5) ? options5.SharedResourceTier : -1},
                          {"d3d12_shared_resource_compatibility_tier",
                           SUCCEEDED(hr4) ? options4.SharedResourceCompatibilityTier : -1},
                          {"d3d12_resource_heap_tier", options12.ResourceHeapTier}};
        Finish(record);
    }
    const DXGI_FORMAT formats[] = {
        DXGI_FORMAT_R8G8B8A8_TYPELESS,    DXGI_FORMAT_R8G8B8A8_UNORM,        DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
        DXGI_FORMAT_B8G8R8A8_TYPELESS,    DXGI_FORMAT_B8G8R8A8_UNORM,        DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
        DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R16G16B16A16_FLOAT,   DXGI_FORMAT_R10G10B10A2_TYPELESS,
        DXGI_FORMAT_R10G10B10A2_UNORM,    DXGI_FORMAT_R16G16_FLOAT,          DXGI_FORMAT_R32_FLOAT,
        DXGI_FORMAT_R32_TYPELESS,         DXGI_FORMAT_R32G8X24_TYPELESS,     DXGI_FORMAT_D32_FLOAT_S8X24_UINT,
        DXGI_FORMAT_R24G8_TYPELESS,       DXGI_FORMAT_D24_UNORM_S8_UINT,     DXGI_FORMAT_D32_FLOAT,
    };
    for (const DXGI_FORMAT format : formats) {
        Record record{"capabilities", FormatName(format), "info"};
        D3D11_FEATURE_DATA_FORMAT_SUPPORT support1{format, 0};
        const HRESULT hr1 = g.device11->CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT, &support1, sizeof(support1));
        D3D11_FEATURE_DATA_FORMAT_SUPPORT2 support2{format, 0};
        const HRESULT hr2 =
            g.device11->CheckFeatureSupport(D3D11_FEATURE_FORMAT_SUPPORT2, &support2, sizeof(support2));
        D3D12_FEATURE_DATA_FORMAT_SUPPORT support12{format};
        const HRESULT hr12 =
            g.device12->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support12, sizeof(support12));
        const bool shareable = SUCCEEDED(hr2) && (support2.OutFormatSupport2 & D3D11_FORMAT_SUPPORT2_SHAREABLE) != 0;
        record.detail = std::string("d3d11 shareable=") + (shareable ? "yes" : "no") +
                        " render_target=" +
                        (SUCCEEDED(hr1) && (support1.OutFormatSupport & D3D11_FORMAT_SUPPORT_RENDER_TARGET) ? "yes" : "no") +
                        " typed_uav=" +
                        (SUCCEEDED(hr1) && (support1.OutFormatSupport & D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW)
                             ? "yes"
                             : "no") +
                        " depth_stencil=" +
                        (SUCCEEDED(hr1) && (support1.OutFormatSupport & D3D11_FORMAT_SUPPORT_DEPTH_STENCIL) ? "yes" : "no") +
                        "; d3d11 support " + (SUCCEEDED(hr1) ? Hex(support1.OutFormatSupport) : HrText(hr1)) +
                        " support2 " + (SUCCEEDED(hr2) ? Hex(support2.OutFormatSupport2) : HrText(hr2)) +
                        "; d3d12 support1 " + (SUCCEEDED(hr12) ? Hex(support12.Support1) : HrText(hr12)) +
                        " support2 " + (SUCCEEDED(hr12) ? Hex(support12.Support2) : HrText(hr12));
        Finish(record);
    }
}

// The comparison itself must notice a difference, or every "pass" below is hollow.
void SelfTest() {
    Record record{"self_test", "comparison_detects_difference"};
    const auto a = PatternBytes(4096, 1, DXGI_FORMAT_R8G8B8A8_TYPELESS);
    auto b = a;
    b[1234] ^= 1;
    const Comparison same = CompareBytes(a, a);
    const Comparison different = CompareBytes(a, b);
    record.status = same.mismatches == 0 && different.mismatches == 1 && different.first == 1234 &&
                            Variety(a) > 1000 && Variety(std::vector<uint8_t>(4096, 7)) == 1
                        ? "pass"
                        : "fail";
    Finish(record);
}

// --- Colour cases ----------------------------------------------------------------------------------------------

struct ColorFormat {
    const char* name;
    DXGI_FORMAT resource;
    std::vector<DXGI_FORMAT> views; // the first is what an app requests
};

const std::vector<ColorFormat>& ColorFormats() {
    static const std::vector<ColorFormat> formats = {
        {"rgba8", DXGI_FORMAT_R8G8B8A8_TYPELESS, {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM}},
        {"bgra8", DXGI_FORMAT_B8G8R8A8_TYPELESS, {DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM}},
        {"bgrx8", DXGI_FORMAT_B8G8R8X8_TYPELESS, {DXGI_FORMAT_B8G8R8X8_UNORM_SRGB, DXGI_FORMAT_B8G8R8X8_UNORM}},
        {"rgba16f", DXGI_FORMAT_R16G16B16A16_TYPELESS, {DXGI_FORMAT_R16G16B16A16_FLOAT}},
        {"rgb10a2", DXGI_FORMAT_R10G10B10A2_TYPELESS, {DXGI_FORMAT_R10G10B10A2_UNORM}},
        // Fully typed: the only formats D3D12's shared-resource tier 0 guarantees across APIs.
        {"rgba8_srgb_typed", DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB}},
        {"bgra8_srgb_typed", DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, {DXGI_FORMAT_B8G8R8A8_UNORM_SRGB}},
        {"rgba16f_typed", DXGI_FORMAT_R16G16B16A16_FLOAT, {DXGI_FORMAT_R16G16B16A16_FLOAT}},
        {"rgb10a2_typed", DXGI_FORMAT_R10G10B10A2_UNORM, {DXGI_FORMAT_R10G10B10A2_UNORM}},
    };
    return formats;
}

std::string CaseName(Lineage lineage, const char* format, const TextureSpec& spec, const char* extra = nullptr) {
    std::string name = std::string(LineageName(lineage)) + "/" + format;
    if (extra) name += std::string("/") + extra;
    return name + "/" + std::to_string(spec.width) + "x" + std::to_string(spec.height);
}

// Direct3D 11 writes known bytes by copy; Direct3D 12 reads them back after the "written" fence.
void BytesCase11To12(const SharedTexture& shared, const TextureSpec& spec, const std::string& name,
                     UINT subresource = 0) {
    Record record{"bytes_11_to_12", name};
    const uint32_t bpp = BytesPerPixel(spec.format);
    const std::size_t size = static_cast<std::size_t>(spec.width) * spec.height * bpp;
    const auto expected = PatternBytes(size, 11, spec.format);
    const auto stale = PatternBytes(size, 12, spec.format);
    const D3D11_TEXTURE2D_DESC desc = Desc11(spec.width, spec.height, spec.format, D3D11_BIND_SHADER_RESOURCE);
    const auto source = CreateTexture11(desc, &expected);
    const auto prefill = CreateTexture11(desc, &stale);
    {
        Access11 access(shared.mutex.Get());
        if (!access.ok()) {
            record.status = "fail";
            record.detail = "AcquireSync " + HrText(access.result());
            Finish(record);
            return;
        }
        g.context11->CopySubresourceRegion(shared.texture11.Get(), subresource, 0, 0, 0, prefill.Get(), 0, nullptr);
        g.context11->CopySubresourceRegion(shared.texture11.Get(), subresource, 0, 0, 0, source.Get(), 0, nullptr);
    }
    Publish11To12();
    ID3D12GraphicsCommandList* list = Begin12();
    Transition(list, shared.texture12.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    const Readback12 readback = RecordReadback12(list, shared.texture12.Get(), subresource);
    Transition(list, shared.texture12.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    End12();
    Publish12To11();
    WaitIdle12();
    const Comparison comparison = ComparePixels(expected, Unpack(readback), spec.format);
    record.status = comparison.mismatches == 0 ? "pass" : "fail";
    if (comparison.mismatches != 0) record.detail = DescribeMismatch(comparison, spec.width, bpp);
    Finish(record);
}

// After D3D11 has written `shared` and Publish11To12 has handed it over: Direct3D 12 reads it three ways, each
// against `expected`, the bytes the D3D11 writes should have left. The raw bytes; a 512x512 window decoded
// through an SRV of `view`, against the same window decoded on D3D11; and a Frame Generation style capture, a
// CopyTextureRegion of an inset box into a private texture of the typed view format (as FG's capture, or the
// presenter copying into a runtime swapchain image, would). Sets the record's status, detail and metrics.
void CheckOnD3D12(const SharedTexture& shared, const TextureSpec& spec, DXGI_FORMAT view,
                  const std::vector<uint8_t>& expected, Record& record) {
    const uint32_t bpp = BytesPerPixel(spec.format);
    const uint32_t window_w = std::min(512u, spec.width);
    const uint32_t window_h = std::min(512u, spec.height);
    const uint32_t window_x = (spec.width - window_w) / 2;
    const uint32_t window_y = (spec.height - window_h) / 2;

    // The decode reference: the expected bytes in a private D3D11 texture, decoded by D3D11.
    const auto holder = CreateTexture11(Desc11(spec.width, spec.height, spec.format, D3D11_BIND_SHADER_RESOURCE),
                                        expected);
    ComPtr<ID3D11ShaderResourceView> holder_srv;
    Must(CreateSrv11(holder.Get(), view, holder_srv), "reference SRV");
    const auto decoded11 = CreateTexture11(
        Desc11(window_w, window_h, DXGI_FORMAT_R32G32B32A32_FLOAT, D3D11_BIND_UNORDERED_ACCESS));
    ComPtr<ID3D11UnorderedAccessView> decoded11_uav;
    Must(CreateUav11(decoded11.Get(), DXGI_FORMAT_R32G32B32A32_FLOAT, decoded11_uav), "decode UAV");
    Dispatch11(g.decode_cs.Get(), holder_srv.Get(), decoded11_uav.Get(), window_w, window_h, window_x, window_y);
    const auto expected_decoded = Read11(decoded11.Get(), 0);

    const auto decoded12 = CreatePrivate12(window_w, window_h, DXGI_FORMAT_R32G32B32A32_FLOAT,
                                           D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const uint32_t box_x = spec.width / 16;
    const uint32_t box_y = spec.height / 16;
    const uint32_t box_w = spec.width - 2 * box_x;
    const uint32_t box_h = spec.height - 2 * box_y;
    const auto capture12 =
        CreatePrivate12(box_w, box_h, view, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    ID3D12GraphicsCommandList* list = Begin12();
    Transition(list, shared.texture12.Get(), D3D12_RESOURCE_STATE_COMMON,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    RecordDecode12(list, DecodeTable12(shared.texture12.Get(), view, decoded12.Get()), window_w, window_h, window_x,
                   window_y);
    Transition(list, shared.texture12.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
               D3D12_RESOURCE_STATE_COPY_SOURCE);
    const Readback12 raw = RecordReadback12(list, shared.texture12.Get(), 0);
    {
        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = capture12.Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION source{};
        source.pResource = shared.texture12.Get();
        source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        const D3D12_BOX box{box_x, box_y, 0, box_x + box_w, box_y + box_h, 1};
        list->CopyTextureRegion(&destination, 0, 0, 0, &source, &box);
    }
    Transition(list, shared.texture12.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    Transition(list, decoded12.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    const Readback12 decoded = RecordReadback12(list, decoded12.Get(), 0);
    Transition(list, capture12.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    const Readback12 captured = RecordReadback12(list, capture12.Get(), 0);
    End12();
    Publish12To11();
    WaitIdle12();

    std::vector<uint8_t> expected_box(static_cast<std::size_t>(box_w) * box_h * bpp);
    for (uint32_t row = 0; row < box_h; ++row) {
        std::memcpy(expected_box.data() + static_cast<std::size_t>(row) * box_w * bpp,
                    expected.data() + (static_cast<std::size_t>(box_y + row) * spec.width + box_x) * bpp,
                    static_cast<std::size_t>(box_w) * bpp);
    }
    const Comparison bytes = ComparePixels(expected, Unpack(raw), spec.format);
    const Comparison values = CompareBytes(expected_decoded, Unpack(decoded));
    const Comparison capture = ComparePixels(expected_box, Unpack(captured), spec.format);
    const std::size_t variety = Variety(expected);
    record.metrics.push_back({"reference_variety", static_cast<double>(variety)});
    if (variety < 64) {
        record.status = "fail";
        record.detail = "reference is nearly blank";
    } else if (bytes.mismatches != 0 || values.mismatches != 0 || capture.mismatches != 0) {
        record.status = "fail";
        record.detail = "bytes: " + (bytes.mismatches ? DescribeMismatch(bytes, spec.width, bpp) : "exact") +
                        "; decoded: " + (values.mismatches ? DescribeMismatch(values, window_w, 16) : "exact") +
                        "; capture copy: " + (capture.mismatches ? DescribeMismatch(capture, box_w, bpp) : "exact");
    } else {
        record.status = "pass";
        record.detail = "bytes, decoded values and capture copy exact";
    }
}

// Direct3D 11 renders through `view` straight into the shared texture (zero copy): a fast clear, a noise draw and
// a partial clear, over stale content. The expected bytes come from the same writes into a private texture.
void RenderCase11To12(const SharedTexture& shared, const TextureSpec& spec, DXGI_FORMAT view,
                      const std::string& name) {
    Record record{"render_11_to_12", name};
    const uint32_t bpp = BytesPerPixel(spec.format);
    const uint32_t seed = 21;
    const auto reference = CreateTexture11(
        Desc11(spec.width, spec.height, spec.format, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE));
    ComPtr<ID3D11RenderTargetView> reference_rtv;
    Must(CreateRtv11(reference.Get(), view, reference_rtv), "reference RTV");
    RenderPattern11(reference_rtv.Get(), spec.width, spec.height, seed);
    const auto expected = Read11(reference.Get(), 0);

    ComPtr<ID3D11RenderTargetView> shared_rtv;
    const HRESULT rtv_hr = CreateRtv11(shared.texture11.Get(), view, shared_rtv);
    if (FAILED(rtv_hr)) {
        record.status = "fail";
        record.detail = "D3D11 RTV on the shared texture " + HrText(rtv_hr);
        Finish(record);
        return;
    }
    const auto stale = CreateTexture11(Desc11(spec.width, spec.height, spec.format, D3D11_BIND_SHADER_RESOURCE),
                                       PatternBytes(static_cast<std::size_t>(spec.width) * spec.height * bpp, 22,
                                                     spec.format));
    {
        Access11 access(shared.mutex.Get());
        if (!access.ok()) {
            record.status = "fail";
            record.detail = "AcquireSync " + HrText(access.result());
            Finish(record);
            return;
        }
        g.context11->CopyResource(shared.texture11.Get(), stale.Get());
        RenderPattern11(shared_rtv.Get(), spec.width, spec.height, seed);
    }
    Publish11To12();
    CheckOnD3D12(shared, spec, view, expected, record);
    Finish(record);
}

// Direct3D 12 writes known bytes by copy from an upload buffer; Direct3D 11 reads after the "done" fence.
void BytesCase12To11(const SharedTexture& shared, const TextureSpec& spec, const std::string& name) {
    Record record{"bytes_12_to_11", name};
    const uint32_t bpp = BytesPerPixel(spec.format);
    const std::size_t size = static_cast<std::size_t>(spec.width) * spec.height * bpp;
    const auto expected = PatternBytes(size, 31, spec.format);
    const auto stale = CreateTexture11(Desc11(spec.width, spec.height, spec.format, D3D11_BIND_SHADER_RESOURCE),
                                       PatternBytes(size, 32, spec.format));
    {
        Access11 access(shared.mutex.Get());
        if (!access.ok()) Abort("AcquireSync " + HrText(access.result()));
        g.context11->CopyResource(shared.texture11.Get(), stale.Get());
    }
    Publish11To12();
    ID3D12GraphicsCommandList* list = Begin12();
    Transition(list, shared.texture12.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    RecordUpload12(list, shared.texture12.Get(), 0, expected);
    Transition(list, shared.texture12.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    End12();
    Publish12To11();
    const Comparison comparison =
        ComparePixels(expected, Read11(shared.texture11.Get(), 0, shared.mutex.Get()), spec.format);
    record.status = comparison.mismatches == 0 ? "pass" : "fail";
    if (comparison.mismatches != 0) record.detail = DescribeMismatch(comparison, spec.width, bpp);
    Finish(record);
}

// Direct3D 12 clears through `view` (whole, then a rectangle): fast-clear state is the classic thing a
// second API fails to see. Direct3D 11 reads against the same clears on a private Direct3D 12 texture.
void ClearCase12To11(const SharedTexture& shared, const TextureSpec& spec, DXGI_FORMAT view,
                     const std::string& name) {
    Record record{"clear_12_to_11", name};
    const uint32_t bpp = BytesPerPixel(spec.format);
    const auto stale = CreateTexture11(Desc11(spec.width, spec.height, spec.format, D3D11_BIND_SHADER_RESOURCE),
                                       PatternBytes(static_cast<std::size_t>(spec.width) * spec.height * bpp, 41,
                                                     spec.format));
    {
        Access11 access(shared.mutex.Get());
        if (!access.ok()) Abort("AcquireSync " + HrText(access.result()));
        g.context11->CopyResource(shared.texture11.Get(), stale.Get());
    }
    Publish11To12();
    const auto reference = CreatePrivate12(spec.width, spec.height, spec.format,
                                           D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);
    const D3D12_RECT rect{static_cast<LONG>(spec.width / 3), static_cast<LONG>(spec.height / 3),
                          static_cast<LONG>(spec.width / 3 + std::max(1u, spec.width / 4)),
                          static_cast<LONG>(spec.height / 3 + std::max(1u, spec.height / 4))};
    ID3D12GraphicsCommandList* list = Begin12();
    for (ID3D12Resource* target : {shared.texture12.Get(), reference.Get()}) {
        Transition(list, target, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv = Rtv12(target, view);
        list->ClearRenderTargetView(rtv, kClearA, 0, nullptr);
        list->ClearRenderTargetView(rtv, kClearB, 1, &rect);
        Transition(list, target, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COMMON);
    }
    Transition(list, reference.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    const Readback12 readback = RecordReadback12(list, reference.Get(), 0);
    End12();
    Publish12To11();
    const auto actual = Read11(shared.texture11.Get(), 0, shared.mutex.Get());
    WaitIdle12();
    const auto expected = Unpack(readback);
    const Comparison comparison = ComparePixels(expected, actual, spec.format);
    const std::size_t variety = Variety(expected);
    if (variety < 2) {
        record.status = "fail";
        record.detail = "reference has one value";
    } else {
        record.status = comparison.mismatches == 0 ? "pass" : "fail";
        if (comparison.mismatches != 0) record.detail = DescribeMismatch(comparison, spec.width, bpp);
    }
    Finish(record);
}

void ColorCase(Lineage lineage, const ColorFormat& format, const Size& size) {
    TextureSpec spec;
    spec.width = size.width;
    spec.height = size.height;
    spec.format = format.resource;
    const std::string base = CaseName(lineage, format.name, spec);
    const SharedTexture shared = CreateShared(lineage, spec);
    {
        Record record{"create", base};
        if (!shared.ok()) {
            record.status = "unsupported";
            record.expect_debug_errors = true;
            record.detail = shared.stage + " " + HrText(shared.hr);
            Finish(record);
            Settle();
            return;
        }
        record.status = "pass";
        DescribeShared(shared, record);
        Finish(record);
    }
    {
        // Every view format on both APIs. Direct3D 12 view creation returns nothing; the debug layer and the
        // render and clear cases below judge it.
        Record record{"views", base};
        std::string failures;
        for (const DXGI_FORMAT view : format.views) {
            ComPtr<ID3D11RenderTargetView> rtv;
            ComPtr<ID3D11ShaderResourceView> srv;
            const HRESULT rtv_hr = CreateRtv11(shared.texture11.Get(), view, rtv);
            const HRESULT srv_hr = CreateSrv11(shared.texture11.Get(), view, srv);
            if (FAILED(rtv_hr)) failures += std::string(" d3d11 RTV ") + FormatName(view) + " " + HrText(rtv_hr);
            if (FAILED(srv_hr)) failures += std::string(" d3d11 SRV ") + FormatName(view) + " " + HrText(srv_hr);
            Rtv12(shared.texture12.Get(), view);
            const UINT index = AllocateSrv12(1);
            D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
            desc.Format = view;
            desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            desc.Texture2D.MipLevels = 1;
            g.device12->CreateShaderResourceView(shared.texture12.Get(), &desc, SrvCpu(index));
        }
        record.status = failures.empty() ? "pass" : "fail";
        record.detail = failures.empty() ? "RTV and SRV for every view format" : failures;
        Finish(record);
    }
    BytesCase11To12(shared, spec, base);
    for (const DXGI_FORMAT view : format.views) {
        RenderCase11To12(shared, spec, view, CaseName(lineage, format.name, spec, FormatName(view)));
    }
    BytesCase12To11(shared, spec, base);
    for (const DXGI_FORMAT view : format.views) {
        ClearCase12To11(shared, spec, view, CaseName(lineage, format.name, spec, FormatName(view)));
    }
    Settle();
}

// --- Depth -----------------------------------------------------------------------------------------------------

struct DepthFormat {
    const char* name;
    DXGI_FORMAT resource;
    DXGI_FORMAT dsv;
    DXGI_FORMAT srv;
};

const std::vector<DepthFormat>& DepthFormats() {
    static const std::vector<DepthFormat> formats = {
        {"d32s8", DXGI_FORMAT_R32G8X24_TYPELESS, DXGI_FORMAT_D32_FLOAT_S8X24_UINT,
         DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS},
        {"d24s8", DXGI_FORMAT_R24G8_TYPELESS, DXGI_FORMAT_D24_UNORM_S8_UINT, DXGI_FORMAT_R24_UNORM_X8_TYPELESS},
        {"d32", DXGI_FORMAT_R32_TYPELESS, DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_R32_FLOAT},
        // Fully typed, so it cannot have an SRV: only direct sharing applies.
        {"d32_typed", DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_UNKNOWN},
    };
    return formats;
}

// Depth content as a game leaves it: a clear, noise depth on the left half, a smooth plane bottom right.
void RenderDepth11(ID3D11DepthStencilView* dsv, const Size& size) {
    g.context11->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
    Constants11 constants;
    constants.seed = 51;
    Draw11(g.depth_ps.Get(), nullptr, dsv, 0, 0, static_cast<float>(size.width / 2), static_cast<float>(size.height),
           constants);
    constants.mode = static_cast<uint32_t>(PatternMode::Smooth);
    Draw11(g.depth_ps.Get(), nullptr, dsv, static_cast<float>(size.width / 2), static_cast<float>(size.height / 2),
           static_cast<float>(size.width - size.width / 2), static_cast<float>(size.height - size.height / 2),
           constants);
}

// Sharing a depth-stencil texture directly: expected to fail (DS cannot be simultaneous-access, and typeless
// depth like DCS's has failed with E_INVALIDARG before). An unexpected success is recorded as such.
void DepthDirectCase(Lineage lineage, const DepthFormat& format, const Size& size) {
    TextureSpec spec;
    spec.width = size.width;
    spec.height = size.height;
    spec.format = format.resource;
    spec.depth_stencil = true;
    spec.render_target = false;
    spec.shader_resource = format.srv != DXGI_FORMAT_UNKNOWN;
    Record record{"depth_direct_share", CaseName(lineage, format.name, spec)};
    const SharedTexture shared = CreateShared(lineage, spec);
    if (!shared.ok()) {
        record.status = "expected_fail";
        record.expect_debug_errors = true;
        record.detail = shared.stage + " " + HrText(shared.hr);
        Finish(record);
        Settle();
        return;
    }
    // It shares after all: does depth written through a DSV arrive intact on D3D12?
    record.status = "unexpected_pass";
    DescribeShared(shared, record);
    const std::string described = record.detail;
    D3D11_DEPTH_STENCIL_VIEW_DESC dsv_desc{};
    dsv_desc.Format = format.dsv;
    dsv_desc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    const auto reference = CreateTexture11(Desc11(size.width, size.height, format.resource, BindFlags11(spec)));
    ComPtr<ID3D11DepthStencilView> reference_dsv;
    Must(g.device11->CreateDepthStencilView(reference.Get(), &dsv_desc, &reference_dsv), "reference DSV");
    RenderDepth11(reference_dsv.Get(), size);
    const auto expected = Read11(reference.Get(), 0);
    ComPtr<ID3D11DepthStencilView> shared_dsv;
    const HRESULT dsv_hr = g.device11->CreateDepthStencilView(shared.texture11.Get(), &dsv_desc, &shared_dsv);
    if (FAILED(dsv_hr)) {
        record.detail = described + "; but no DSV on it: " + HrText(dsv_hr);
        Finish(record);
        Settle();
        return;
    }
    {
        Access11 access(shared.mutex.Get());
        if (!access.ok()) Abort("AcquireSync " + HrText(access.result()));
        RenderDepth11(shared_dsv.Get(), size);
    }
    Publish11To12();
    ID3D12GraphicsCommandList* list = Begin12();
    Transition(list, shared.texture12.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    const Readback12 readback = RecordReadback12(list, shared.texture12.Get(), 0);
    Transition(list, shared.texture12.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    End12();
    Publish12To11();
    WaitIdle12();
    const Comparison comparison = CompareBytes(expected, Unpack(readback));
    record.detail = described + (comparison.mismatches == 0
                                     ? "; depth written through a DSV arrives exact on D3D12"
                                     : "; depth data wrong on D3D12: " + DescribeMismatch(comparison, size.width, 4));
    if (comparison.mismatches != 0) record.status = "fail";
    Finish(record);
    Settle();
}

// The path the bridge would use for an app's depth swapchain: a private depth texture, rendered with clears and
// draws, copied by a Direct3D 11 compute pass into a shared R32_FLOAT texture that Direct3D 12 reads.
void DepthCopyCase(Lineage lineage, const DepthFormat& format, const Size& size) {
    if (format.srv == DXGI_FORMAT_UNKNOWN) return;
    TextureSpec spec;
    spec.width = size.width;
    spec.height = size.height;
    spec.format = DXGI_FORMAT_R32_FLOAT;
    // Render target as well as UAV: Direct3D 11 opens no D3D12-owned texture without ALLOW_RENDER_TARGET.
    spec.unordered_access = true;
    Record record{"depth_copy_r32f", CaseName(lineage, format.name, spec)};

    const auto depth = CreateTexture11(Desc11(size.width, size.height, format.resource,
                                              D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE));
    D3D11_DEPTH_STENCIL_VIEW_DESC dsv_desc{};
    dsv_desc.Format = format.dsv;
    dsv_desc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    ComPtr<ID3D11DepthStencilView> dsv;
    Must(g.device11->CreateDepthStencilView(depth.Get(), &dsv_desc, &dsv), "DSV");
    ComPtr<ID3D11ShaderResourceView> depth_srv;
    Must(CreateSrv11(depth.Get(), format.srv, depth_srv), "depth SRV");
    RenderDepth11(dsv.Get(), size);

    const auto reference = CreateTexture11(Desc11(size.width, size.height, DXGI_FORMAT_R32_FLOAT,
                                                  D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS));
    ComPtr<ID3D11UnorderedAccessView> reference_uav;
    Must(CreateUav11(reference.Get(), DXGI_FORMAT_R32_FLOAT, reference_uav), "reference UAV");
    Dispatch11(g.depth_copy_cs.Get(), depth_srv.Get(), reference_uav.Get(), size.width, size.height);
    const auto expected = Read11(reference.Get(), 0);

    const SharedTexture shared = CreateShared(lineage, spec);
    if (!shared.ok()) {
        record.status = "unsupported";
        record.expect_debug_errors = true;
        record.detail = "shared R32_FLOAT: " + shared.stage + " " + HrText(shared.hr);
        Finish(record);
        Settle();
        return;
    }
    ComPtr<ID3D11UnorderedAccessView> shared_uav;
    const HRESULT uav_hr = CreateUav11(shared.texture11.Get(), DXGI_FORMAT_R32_FLOAT, shared_uav);
    if (FAILED(uav_hr)) {
        record.status = "fail";
        record.detail = "D3D11 UAV on the shared R32_FLOAT " + HrText(uav_hr);
        Finish(record);
        Settle();
        return;
    }
    const auto stale = CreateTexture11(Desc11(size.width, size.height, DXGI_FORMAT_R32_FLOAT,
                                              D3D11_BIND_SHADER_RESOURCE),
                                       PatternBytes(static_cast<std::size_t>(size.width) * size.height * 4, 52,
                                                     DXGI_FORMAT_R32_FLOAT));
    {
        Access11 access(shared.mutex.Get());
        if (!access.ok()) Abort("AcquireSync " + HrText(access.result()));
        g.context11->CopyResource(shared.texture11.Get(), stale.Get());
        Dispatch11(g.depth_copy_cs.Get(), depth_srv.Get(), shared_uav.Get(), size.width, size.height);
    }
    Publish11To12();
    ID3D12GraphicsCommandList* list = Begin12();
    Transition(list, shared.texture12.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    const Readback12 readback = RecordReadback12(list, shared.texture12.Get(), 0);
    Transition(list, shared.texture12.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    End12();
    Publish12To11();
    WaitIdle12();
    const Comparison comparison = CompareBytes(expected, Unpack(readback));
    const std::size_t variety = Variety(expected);
    record.metrics = {{"reference_variety", static_cast<double>(variety)}};
    if (variety < 64) {
        record.status = "fail";
        record.detail = "reference depth is nearly blank";
    } else {
        record.status = comparison.mismatches == 0 ? "pass" : "fail";
        record.detail = comparison.mismatches == 0 ? "float bits exact" : DescribeMismatch(comparison, size.width, 4);
    }
    Finish(record);
    Settle();
}

// --- Shapes: arrays, UAV usage, MSAA ---------------------------------------------------------------------------

// DCS's typeless backing, except for B_plain: Direct3D 11 opens a D3D12-owned texture without simultaneous
// access only in a fully typed format.
DXGI_FORMAT ProtocolFormat(Lineage lineage) {
    return lineage == Lineage::D3D12Plain ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_TYPELESS;
}

const char* ProtocolFormatName(Lineage lineage) {
    return lineage == Lineage::D3D12Plain ? "rgba8_srgb_typed" : "rgba8";
}

void ShapeCases(Lineage lineage, const Size& size) {
    const bool plain = lineage == Lineage::D3D12Plain;
    {
        // A two-slice array, as a stereo app's single swapchain: bytes through slice 1.
        TextureSpec spec;
        spec.width = size.width;
        spec.height = size.height;
        spec.format = ProtocolFormat(lineage);
        spec.array_size = 2;
        const std::string name = CaseName(lineage, ProtocolFormatName(lineage), spec, "array2");
        const SharedTexture shared = CreateShared(lineage, spec);
        Record record{"create", name};
        record.status = shared.ok() ? "pass" : "unsupported";
        record.expect_debug_errors = !shared.ok();
        if (shared.ok()) {
            DescribeShared(shared, record);
        } else {
            record.detail = shared.stage + " " + HrText(shared.hr);
        }
        Finish(record);
        if (shared.ok()) BytesCase11To12(shared, spec, name + "/slice1", 1);
        Settle();
    }
    {
        // Unordered access, as an app asking for XR_SWAPCHAIN_USAGE_UNORDERED_ACCESS_BIT. sRGB formats cannot be
        // UAVs, so B_plain uses the typed UNORM format.
        TextureSpec spec;
        spec.width = size.width;
        spec.height = size.height;
        spec.format = plain ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_R8G8B8A8_TYPELESS;
        spec.unordered_access = true;
        const std::string name = CaseName(lineage, plain ? "rgba8_unorm_typed" : "rgba8", spec, "uav");
        const SharedTexture shared = CreateShared(lineage, spec);
        Record record{"create", name};
        record.status = shared.ok() ? "pass" : "unsupported";
        record.expect_debug_errors = !shared.ok();
        if (shared.ok()) {
            DescribeShared(shared, record);
        } else {
            record.detail = shared.stage + " " + HrText(shared.hr);
        }
        Finish(record);
        if (shared.ok()) BytesCase11To12(shared, spec, name);
        Settle();
    }
    {
        // 4x MSAA: simultaneous access forbids it; the others are informational.
        TextureSpec spec;
        spec.width = size.width;
        spec.height = size.height;
        spec.format = ProtocolFormat(lineage);
        spec.samples = 4;
        Record record{"create_msaa4", CaseName(lineage, ProtocolFormatName(lineage), spec)};
        const SharedTexture shared = CreateShared(lineage, spec);
        record.status = shared.ok() ? "info" : (lineage == Lineage::D3D12Simultaneous ? "expected_fail" : "info");
        record.expect_debug_errors = !shared.ok();
        if (shared.ok()) {
            DescribeShared(shared, record);
            record.detail = "created; " + record.detail;
        } else {
            record.detail = "not shareable: " + shared.stage + " " + HrText(shared.hr);
        }
        Finish(record);
        Settle();
    }
}

// --- Copy at release: private app images copied into a shared ring slot -----------------------------------------

// The harvest's recommended bridge. The app renders its own private image (DCS: R8G8B8A8_TYPELESS through an
// sRGB RTV, so it may be compressed and fast-cleared), and at release the layer copies it into a shared slot
// that D3D12 reads. Also slice 1 of a two-slice array (a stereo app's single swapchain) and a 4x MSAA image
// resolved into the slot. B_plain's slot is fully typed, so its copies are typeless-to-typed.
void CopyAtReleaseCase(Lineage lineage, const Size& size) {
    constexpr DXGI_FORMAT kAppFormat = DXGI_FORMAT_R8G8B8A8_TYPELESS;
    constexpr DXGI_FORMAT kView = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    const uint32_t w = size.width;
    const uint32_t h = size.height;
    TextureSpec spec;
    spec.width = w;
    spec.height = h;
    spec.format = ProtocolFormat(lineage);
    const std::string slot_name = std::string(ProtocolFormatName(lineage)) + "_slot";
    const SharedTexture shared = CreateShared(lineage, spec);
    if (!shared.ok()) {
        Record record{"copy_at_release", CaseName(lineage, slot_name.c_str(), spec), "unsupported"};
        record.expect_debug_errors = true;
        record.detail = shared.stage + " " + HrText(shared.hr);
        Finish(record);
        Settle();
        return;
    }
    const UINT bind = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    const auto image = CreateTexture11(Desc11(w, h, kAppFormat, bind));
    {
        ComPtr<ID3D11RenderTargetView> rtv;
        Must(CreateRtv11(image.Get(), kView, rtv), "app RTV");
        RenderPattern11(rtv.Get(), w, h, 400);
    }
    const auto array = CreateTexture11(Desc11(w, h, kAppFormat, bind, 2));
    for (UINT slice = 0; slice < 2; ++slice) {
        D3D11_RENDER_TARGET_VIEW_DESC desc{};
        desc.Format = kView;
        desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
        desc.Texture2DArray.FirstArraySlice = slice;
        desc.Texture2DArray.ArraySize = 1;
        ComPtr<ID3D11RenderTargetView> rtv;
        Must(g.device11->CreateRenderTargetView(array.Get(), &desc, &rtv), "slice RTV");
        RenderPattern11(rtv.Get(), w, h, 401 + slice);
    }
    const auto msaa = CreateTexture11(Desc11(w, h, kAppFormat, D3D11_BIND_RENDER_TARGET, 1, 4));
    {
        ComPtr<ID3D11RenderTargetView> rtv;
        Must(CreateRtv11(msaa.Get(), kView, rtv), "MSAA RTV");
        RenderPattern11(rtv.Get(), w, h, 403);
    }
    const auto resolved = CreateTexture11(Desc11(w, h, kAppFormat, D3D11_BIND_SHADER_RESOURCE));
    g.context11->ResolveSubresource(resolved.Get(), 0, msaa.Get(), 0, kView);
    const auto stale =
        CreateTexture11(Desc11(w, h, spec.format, D3D11_BIND_SHADER_RESOURCE),
                        PatternBytes(static_cast<std::size_t>(w) * h * 4, 404, spec.format));

    struct Source {
        const char* name;
        std::vector<uint8_t> expected;
        std::function<void()> copy;
    };
    std::vector<Source> sources;
    sources.push_back({"image", Read11(image.Get(), 0),
                       [&] { g.context11->CopyResource(shared.texture11.Get(), image.Get()); }});
    sources.push_back({"array_slice1", Read11(array.Get(), 1), [&] {
                           g.context11->CopySubresourceRegion(shared.texture11.Get(), 0, 0, 0, 0, array.Get(), 1,
                                                              nullptr);
                       }});
    sources.push_back({"msaa4_resolve", Read11(resolved.Get(), 0), [&] {
                           g.context11->ResolveSubresource(shared.texture11.Get(), 0, msaa.Get(), 0, kView);
                       }});
    for (Source& source : sources) {
        Record record{"copy_at_release", CaseName(lineage, slot_name.c_str(), spec, source.name)};
        {
            Access11 access(shared.mutex.Get());
            if (!access.ok()) Abort("AcquireSync " + HrText(access.result()));
            g.context11->CopyResource(shared.texture11.Get(), stale.Get());
            source.copy();
        }
        Publish11To12();
        CheckOnD3D12(shared, spec, kView, source.expected, record);
        Finish(record);
    }
    Settle();
}

// --- Creation matrix: which flag combinations share at all ----------------------------------------------------

// Creation and open only, per colour format, for flag combinations beyond the four lineages. It settles which
// flags Direct3D 11 needs to open a D3D12-owned texture (simultaneous access, render target, UAV) and whether
// it accepts an NT handle with neither SHARED nor a keyed mutex.
void CreationMatrix(const Size& size) {
    struct Variant {
        const char* name;
        UINT misc11;
        UINT bind11;
        D3D12_RESOURCE_FLAGS flags12;
    };
    constexpr UINT kSrv = D3D11_BIND_SHADER_RESOURCE;
    constexpr UINT kSrvRt = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    const Variant variants[] = {
        {"A_nt_handle_only", D3D11_RESOURCE_MISC_SHARED_NTHANDLE, kSrvRt, D3D12_RESOURCE_FLAG_NONE},
        {"A_keyed_mutex_srv_only", D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX, kSrv,
         D3D12_RESOURCE_FLAG_NONE},
        {"A_nt_shared_srv_only", D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED, kSrv,
         D3D12_RESOURCE_FLAG_NONE},
        {"B_no_flags", 0, 0, D3D12_RESOURCE_FLAG_NONE},
        {"B_simultaneous_only", 0, 0, D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS},
        {"B_render_target_only", 0, 0, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET},
        {"B_render_target_simultaneous", 0, 0,
         D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS},
        {"B_uav_only", 0, 0, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS},
        {"B_render_target_uav", 0, 0, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS},
        {"B_render_target_uav_simultaneous", 0, 0,
         D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS |
             D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS},
    };
    // The colour formats, plus typed formats that can be UAVs (sRGB and X8 cannot), and the depth bridge's R32F.
    std::vector<std::pair<std::string, DXGI_FORMAT>> formats;
    for (const ColorFormat& format : ColorFormats()) formats.push_back({format.name, format.resource});
    formats.push_back({"rgba8_unorm_typed", DXGI_FORMAT_R8G8B8A8_UNORM});
    formats.push_back({"r32f", DXGI_FORMAT_R32_FLOAT});
    formats.push_back({"rg16f", DXGI_FORMAT_R16G16_FLOAT});
    for (const auto& [format_name, format] : formats) {
        for (const Variant& variant : variants) {
            TextureSpec spec;
            spec.width = size.width;
            spec.height = size.height;
            spec.format = format;
            Record record{"creation_matrix", std::string(variant.name) + "/" + format_name, "info"};
            record.expect_debug_errors = true;
            const SharedTexture shared = CreateSharedWith(variant.misc11, variant.bind11, variant.flags12, spec);
            if (shared.ok()) {
                DescribeShared(shared, record);
                record.detail = "shares; " + record.detail;
            } else {
                record.detail = "fails at " + shared.stage + " " + HrText(shared.hr);
            }
            Finish(record);
        }
        Settle();
    }
}

// --- Resting state: the Frame Generation backend's assumption --------------------------------------------------

// FG's D3D12 capture treats app images as resting in RENDER_TARGET (XR_KHR_D3D12_enable); shared textures are
// meant to be in COMMON at every cross-API handoff. For a simultaneous-access texture (every D3D11-created one
// opens that way, and B_simultaneous) the difference is moot by spec: it decays to COMMON after each
// ExecuteCommandLists, and RENDER_TARGET is a legal before-state from COMMON. Only a non-simultaneous texture
// (B_plain) can break, so each frame gives it something stale to trip over: D3D12 fast-clears it to its optimized
// clear value, D3D11 then overwrites all of it, and D3D12 reads it. Run twice: with the texture handed over in
// COMMON (the protocol, which must be exact) and resting in RENDER_TARGET across the D3D11 writes (FG's view).
void RestingStateCase(Lineage lineage, const Size& size) {
    constexpr DXGI_FORMAT kView = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    constexpr float kZero[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    TextureSpec spec;
    spec.width = size.width;
    spec.height = size.height;
    spec.format = ProtocolFormat(lineage);
    Record record{"resting_render_target", CaseName(lineage, ProtocolFormatName(lineage), spec), "info"};
    D3D12_CLEAR_VALUE clear{};
    clear.Format = kView;
    std::memcpy(clear.Color, kZero, sizeof(kZero));
    SharedTexture shared;
    if (lineage == Lineage::D3D12Plain) {
        shared = CreateSharedWith(0, 0, Flags12(spec), spec, &clear);
    } else if (lineage == Lineage::D3D12Simultaneous) {
        shared = CreateSharedWith(0, 0, Flags12(spec) | D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS, spec, &clear);
    } else {
        shared = CreateShared(lineage, spec);
    }
    if (!shared.ok()) {
        record.status = "unsupported";
        record.expect_debug_errors = true;
        record.detail = shared.stage + " " + HrText(shared.hr);
        Finish(record);
        Settle();
        return;
    }
    const bool simultaneous =
        (shared.texture12->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS) != 0;
    ComPtr<ID3D11RenderTargetView> rtv;
    Must(CreateRtv11(shared.texture11.Get(), kView, rtv), "RTV");
    const auto reference = CreateTexture11(
        Desc11(spec.width, spec.height, spec.format, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE));
    ComPtr<ID3D11RenderTargetView> reference_rtv;
    Must(CreateRtv11(reference.Get(), kView, reference_rtv), "reference RTV");
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv12 = Rtv12(shared.texture12.Get(), kView);

    auto run = [&](bool rest_in_render_target, uint32_t seed) {
        const D3D12_RESOURCE_STATES rest =
            rest_in_render_target ? D3D12_RESOURCE_STATE_RENDER_TARGET : D3D12_RESOURCE_STATE_COMMON;
        uint32_t mismatched = 0;
        std::string first;
        ID3D12GraphicsCommandList* list = Begin12();
        Transition(list, shared.texture12.Get(), D3D12_RESOURCE_STATE_COMMON, rest);
        End12();
        for (uint32_t frame = 0; frame < 3; ++frame) {
            list = Begin12();
            Transition(list, shared.texture12.Get(), rest, D3D12_RESOURCE_STATE_RENDER_TARGET);
            list->ClearRenderTargetView(rtv12, kZero, 0, nullptr);
            Transition(list, shared.texture12.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, rest);
            End12();
            Publish12To11();
            RenderPattern11(reference_rtv.Get(), spec.width, spec.height, seed + frame);
            const auto expected = Read11(reference.Get(), 0);
            {
                Access11 access(shared.mutex.Get());
                if (!access.ok()) Abort("AcquireSync " + HrText(access.result()));
                RenderPattern11(rtv.Get(), spec.width, spec.height, seed + frame);
            }
            Publish11To12();
            list = Begin12();
            Transition(list, shared.texture12.Get(), rest, D3D12_RESOURCE_STATE_COPY_SOURCE);
            const Readback12 readback = RecordReadback12(list, shared.texture12.Get(), 0);
            Transition(list, shared.texture12.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, rest);
            End12();
            Publish12To11();
            WaitIdle12();
            const Comparison comparison = CompareBytes(expected, Unpack(readback));
            if (comparison.mismatches != 0 && mismatched++ == 0) {
                first = "frame " + std::to_string(frame) + ": " + DescribeMismatch(comparison, spec.width, 4);
            }
        }
        list = Begin12();
        Transition(list, shared.texture12.Get(), rest, D3D12_RESOURCE_STATE_COMMON);
        End12();
        Publish12To11();
        return std::make_pair(mismatched, first);
    };
    const auto [common_mismatched, common_first] = run(false, 310);
    const auto [resting_mismatched, resting_first] = run(true, 320);
    record.metrics = {{"simultaneous_access", simultaneous ? 1.0 : 0.0},
                      {"mismatched_frames_common_handoff", common_mismatched},
                      {"mismatched_frames_resting_render_target", resting_mismatched}};
    if (common_mismatched != 0) {
        record.status = "fail";
        record.detail = "the COMMON handoff itself is wrong: " + common_first;
    } else if (simultaneous) {
        record.detail = std::string("simultaneous access: decays to COMMON after every ExecuteCommandLists, so ") +
                        "resting in RENDER_TARGET is legal by spec; data " +
                        (resting_mismatched == 0 ? "exact" : "WRONG (" + resting_first + ")");
    } else {
        record.detail = resting_mismatched == 0
                            ? "not simultaneous: resting in RENDER_TARGET across D3D11 writes happened to work; "
                              "the spec still requires COMMON at the handoff"
                            : "not simultaneous: resting in RENDER_TARGET breaks (" + resting_first + ")";
    }
    Finish(record);
    Settle();
}

// The debug layers' silence about the resting-state barriers means something only if they catch a wrong
// before-state at all. A private, non-simultaneous texture sits in COMMON; a barrier claims it was in
// RENDER_TARGET. Only the queue-level state tracking that comes with synchronized queue validation checks this,
// so it is the --gpu-validation run that must report it (a pass), or the resting cases prove nothing.
void DebugLayerControl() {
    if (!g_options.debug) return;
    Record record{"debug_layer_control", "wrong_before_state_on_private_texture"};
    record.expect_debug_errors = true;
    const auto texture = CreatePrivate12(64, 64, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                                         D3D12_RESOURCE_STATE_COMMON);
    ID3D12GraphicsCommandList* list = Begin12();
    Transition(list, texture.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Transition(list, texture.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    End12();
    WaitIdle12();
    DrainDebug(record);
    if (record.debug_error) {
        record.status = "pass";
        record.detail = "the debug layers report a wrong before-state, so their silence elsewhere counts";
    } else if (g_options.gpu_validation) {
        record.status = "insensitive";
        record.detail = "no report for a wrong before-state: the resting-state silence proves nothing";
    } else {
        record.status = "info";
        record.detail = "not reported: barrier before-states are checked only with synchronized queue validation "
                        "(--gpu-validation), so this run's silence about states proves nothing";
    }
    Finish(record);
    Settle();
}

// --- Keyed mutex: what an unacquired Direct3D 11 write does ----------------------------------------------------

// A D3D11 write to a keyed-mutex texture without AcquireSync has been seen to lose its data with every HRESULT
// successful. Reproduced here so the rule (always acquire, check for S_OK) rests on a measurement.
void UnacquiredWriteCase(const Size& size) {
    TextureSpec spec;
    spec.width = size.width;
    spec.height = size.height;
    spec.format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
    Record record{"keyed_mutex_unacquired_write", CaseName(Lineage::D3D11KeyedMutex, "rgba8", spec)};
    record.expect_debug_errors = true;
    const SharedTexture shared = CreateShared(Lineage::D3D11KeyedMutex, spec);
    if (!shared.ok()) {
        record.status = "fail";
        record.detail = shared.stage + " " + HrText(shared.hr);
        Finish(record);
        Settle();
        return;
    }
    const std::size_t bytes = static_cast<std::size_t>(spec.width) * spec.height * 4;
    const auto expected = PatternBytes(bytes, 61, spec.format);
    const auto source =
        CreateTexture11(Desc11(spec.width, spec.height, spec.format, D3D11_BIND_SHADER_RESOURCE), &expected);
    g.context11->CopyResource(shared.texture11.Get(), source.Get());
    Publish11To12();
    ID3D12GraphicsCommandList* list = Begin12();
    Transition(list, shared.texture12.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    const Readback12 readback = RecordReadback12(list, shared.texture12.Get(), 0);
    Transition(list, shared.texture12.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    End12();
    Publish12To11();
    WaitIdle12();
    const Comparison comparison = CompareBytes(expected, Unpack(readback));
    record.status = comparison.mismatches != 0 ? "expected_fail" : "unexpected_pass";
    record.detail = comparison.mismatches != 0 ? "the write never arrived: " + DescribeMismatch(comparison, spec.width, 4)
                                               : "the unacquired write arrived intact";
    Finish(record);
    Settle();
}

// --- Fences ----------------------------------------------------------------------------------------------------

// The D3D12 queue must hold behind a D3D11 signal it has not seen, then release once D3D11 signals and flushes.
void GateTest12Waits11(const std::string& name, ID3D12Fence* fence12, ID3D11Fence* fence11, uint64_t value) {
    Record record{"fences", name};
    Must(g.queue12->Wait(fence12, value), "queue Wait");
    const uint64_t marker = Mark12();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const bool held = g.idle12->GetCompletedValue() < marker;
    const double start = NowMs();
    Must(g.context11->Signal(fence11, value), "context Signal");
    g.context11->Flush();
    bool released = false;
    while (NowMs() - start < kPollTimeoutMs) {
        if (g.idle12->GetCompletedValue() >= marker) {
            released = true;
            break;
        }
        std::this_thread::yield();
    }
    const double latency = NowMs() - start;
    record.status = held && released ? "pass" : "fail";
    record.detail = std::string(held ? "held" : "NOT HELD") + " for 50 ms, " +
                    (released ? "released" : "NOT RELEASED") + " after the D3D11 signal";
    record.metrics = {{"release_latency_ms", latency}};
    Finish(record);
    Settle();
}

// The D3D11 context must hold behind a D3D12 signal it has not seen, then release once D3D12 signals.
void GateTest11Waits12(const std::string& name, ID3D11Fence* fence11, ID3D12Fence* fence12, uint64_t value) {
    Record record{"fences", name};
    const double call_start = NowMs();
    Must(g.context11->Wait(fence11, value), "context Wait");
    const double wait_call_ms = NowMs() - call_start;
    D3D11_QUERY_DESC desc{D3D11_QUERY_EVENT, 0};
    ComPtr<ID3D11Query> event;
    Must(g.device11->CreateQuery(&desc, &event), "CreateQuery");
    g.context11->End(event.Get());
    g.context11->Flush();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    BOOL done = FALSE;
    const bool held = g.context11->GetData(event.Get(), &done, sizeof(done), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_FALSE;
    const double start = NowMs();
    Must(g.queue12->Signal(fence12, value), "queue Signal");
    bool released = false;
    while (NowMs() - start < kPollTimeoutMs) {
        if (g.context11->GetData(event.Get(), &done, sizeof(done), 0) == S_OK) {
            released = true;
            break;
        }
        std::this_thread::yield();
    }
    const double latency = NowMs() - start;
    record.status = held && released ? "pass" : "fail";
    record.detail = std::string(held ? "held" : "NOT HELD") + " for 50 ms, " +
                    (released ? "released" : "NOT RELEASED") + " after the D3D12 signal";
    record.metrics = {{"release_latency_ms", latency}, {"d3d11_wait_call_ms", wait_call_ms}};
    Finish(record);
    Settle();
}

// Whether a D3D11 signal reaches D3D12 without an explicit Flush, with unflushed D3D11 work queued in front of
// it (informational: the bridge always flushes).
void SignalWithoutFlush() {
    Record record{"fences", "d3d11_signal_without_flush", "info"};
    const auto target = CreateTexture11(Desc11(1024, 1024, DXGI_FORMAT_R16G16B16A16_FLOAT,
                                               D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE));
    ComPtr<ID3D11RenderTargetView> rtv;
    Must(CreateRtv11(target.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, rtv), "RTV");
    WaitIdle11();
    Constants11 constants;
    constants.iterations = 16;
    for (int i = 0; i < 4; ++i) Draw11(g.pattern_ps.Get(), rtv.Get(), nullptr, 0, 0, 1024, 1024, constants);
    const uint64_t value = ++g_to12.value;
    Must(g.context11->Signal(g_to12.fence11.Get(), value), "context Signal");
    Must(g.queue12->Wait(g_to12.fence12.Get(), value), "queue Wait");
    const uint64_t marker = Mark12();
    const double start = NowMs();
    bool observed = false;
    while (NowMs() - start < 500.0) {
        if (g.idle12->GetCompletedValue() >= marker) {
            observed = true;
            break;
        }
        std::this_thread::yield();
    }
    const double latency = NowMs() - start;
    g.context11->Flush();
    record.detail = observed ? "D3D12 saw the signal without a flush" : "D3D12 did not see it within 500 ms";
    record.metrics = {{"observed_ms", observed ? latency : -1.0}};
    Finish(record);
    Settle();
}

void FenceCases() {
    GateTest12Waits11("gate_d3d12_waits_on_d3d11", g_to12.fence12.Get(), g_to12.fence11.Get(), ++g_to12.value);
    GateTest11Waits12("gate_d3d11_waits_on_d3d12", g_to11.fence11.Get(), g_to11.fence12.Get(), ++g_to11.value);
    SignalWithoutFlush();

    // The other creation direction: D3D11 creates and shares the fence, D3D12 opens it.
    Record record{"fences", "create_d3d11_shared_open_d3d12"};
    ComPtr<ID3D11Fence> fence11;
    ComPtr<ID3D12Fence> fence12;
    HANDLE handle = nullptr;
    HRESULT hr = g.device11->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence11));
    std::string stage = "d3d11_create";
    if (SUCCEEDED(hr)) {
        stage = "d3d11_create_shared_handle";
        hr = fence11->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &handle);
    }
    if (SUCCEEDED(hr)) {
        stage = "d3d12_open";
        hr = g.device12->OpenSharedHandle(handle, IID_PPV_ARGS(&fence12));
        CloseHandle(handle);
    }
    record.status = SUCCEEDED(hr) ? "pass" : "fail";
    record.detail = SUCCEEDED(hr) ? "opened" : stage + " " + HrText(hr);
    Finish(record);
    if (SUCCEEDED(hr)) GateTest12Waits11("gate_d3d12_waits_on_d3d11_created_fence", fence12.Get(), fence11.Get(), 1);
}

// --- Removal: the D3D12 device dies while D3D11 waits on it --------------------------------------------------

// The bridge's failure mode: FG's D3D12 device is removed while the game's D3D11 context has a queued Wait on a
// "done" value D3D12 will now never signal. Does that wait release (the fence going to UINT64_MAX), or does it
// hang the game's context? And can D3D11 still use the shared textures of each ownership direction afterwards?
// Irreversible, so it runs alone (--removal).
void RemovalCase() {
    const Size size = kFocus;
    TextureSpec spec;
    spec.width = size.width;
    spec.height = size.height;
    spec.format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
    constexpr DXGI_FORMAT kView = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    struct Owned {
        Lineage lineage;
        SharedTexture shared;
        ComPtr<ID3D11RenderTargetView> rtv;
    };
    std::vector<Owned> textures;
    for (const Lineage lineage : {Lineage::D3D11Shared, Lineage::D3D12Simultaneous}) {
        Owned owned{lineage, CreateShared(lineage, spec), nullptr};
        if (!owned.shared.ok()) Abort(std::string("removal: cannot create ") + LineageName(lineage));
        Must(CreateRtv11(owned.shared.texture11.Get(), kView, owned.rtv), "RTV");
        RenderPattern11(owned.rtv.Get(), size.width, size.height, 500);
        textures.push_back(owned);
    }
    Publish11To12();
    Publish12To11();
    WaitIdle12();
    WaitIdle11();

    Record wait_record{"removal", "d3d11_wait_on_removed_d3d12_device", "info"};
    const uint64_t never = g_to11.value + 1;
    Must(g.context11->Wait(g_to11.fence11.Get(), never), "context Wait");
    D3D11_QUERY_DESC desc{D3D11_QUERY_EVENT, 0};
    ComPtr<ID3D11Query> event;
    Must(g.device11->CreateQuery(&desc, &event), "CreateQuery");
    g.context11->End(event.Get());
    g.context11->Flush();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    BOOL done = FALSE;
    const bool held = g.context11->GetData(event.Get(), &done, sizeof(done), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_FALSE;
    ComPtr<ID3D12Device5> device5;
    Must(g.device12.As(&device5), "ID3D12Device5");
    const double start = NowMs();
    device5->RemoveDevice();
    bool released = false;
    while (NowMs() - start < 3000.0) {
        if (g.context11->GetData(event.Get(), &done, sizeof(done), 0) == S_OK) {
            released = true;
            break;
        }
        std::this_thread::yield();
    }
    const double latency = NowMs() - start;
    const uint64_t completed11 = g_to11.fence11->GetCompletedValue();
    const uint64_t completed12 = g_to11.fence12->GetCompletedValue();
    wait_record.detail = std::string(held ? "held before removal; " : "NOT held before removal; ") +
                         (released ? "released by the removal" : "STILL HELD 3 s after the removal") +
                         "; fence as seen by D3D11 " + Hex(completed11, 16) + ", by D3D12 " + Hex(completed12, 16) +
                         "; d3d12 removed reason " + HrText(g.device12->GetDeviceRemovedReason()) +
                         ", d3d11 removed reason " + HrText(g.device11->GetDeviceRemovedReason());
    wait_record.metrics = {{"release_latency_ms", released ? latency : -1.0}};
    Finish(wait_record);
    if (!released) {
        // The context is stuck behind the wait, so nothing below can run.
        return;
    }
    for (Owned& owned : textures) {
        Record record{"removal", std::string("d3d11_uses_") + LineageName(owned.lineage) + "_after_removal", "info"};
        constexpr float kGreen[4] = {0.0f, 1.0f, 0.0f, 1.0f};
        g.context11->ClearRenderTargetView(owned.rtv.Get(), kGreen);
        const auto bytes = Read11(owned.shared.texture11.Get(), 0);
        std::size_t wrong = 0;
        for (std::size_t i = 0; i + 3 < bytes.size(); i += 4) {
            wrong += !(bytes[i] == 0 && bytes[i + 1] == 255 && bytes[i + 2] == 0 && bytes[i + 3] == 255);
        }
        record.detail = wrong == 0 ? "clear and readback on D3D11 still work"
                                   : std::to_string(wrong) + " pixels wrong after a D3D11 clear and readback";
        record.detail += "; d3d11 removed reason " + HrText(g.device11->GetDeviceRemovedReason());
        Finish(record);
    }
}

// --- Teardown: a D3D11 wait whose D3D12 signal never comes ------------------------------------------------------

// A D3D11 wait on a "done" value nobody signals: does it hold the game's context indefinitely (fence waits do not
// time out), and what releases it? First a CPU-side ID3D12Fence::Signal, the bridge's teardown tool. Then the
// question that decides the teardown rule: does simply releasing every D3D12 object release a pending wait?
// Irreversible if the answer is no, so it runs alone (--teardown).
bool PollEvent11(ID3D11Query* event, double milliseconds, double* latency) {
    const double start = NowMs();
    BOOL done = FALSE;
    while (NowMs() - start < milliseconds) {
        if (g.context11->GetData(event, &done, sizeof(done), 0) == S_OK) {
            if (latency) *latency = NowMs() - start;
            return true;
        }
        std::this_thread::yield();
    }
    return false;
}

ComPtr<ID3D11Query> QueueEventAfterWait11(ID3D11Fence* fence, uint64_t value) {
    Must(g.context11->Wait(fence, value), "context Wait");
    D3D11_QUERY_DESC desc{D3D11_QUERY_EVENT, 0};
    ComPtr<ID3D11Query> event;
    Must(g.device11->CreateQuery(&desc, &event), "CreateQuery");
    g.context11->End(event.Get());
    g.context11->Flush();
    return event;
}

void TeardownCase() {
    {
        Record record{"teardown", "d3d11_wait_never_signalled_then_cpu_signal", "info"};
        const uint64_t value = ++g_to11.value;
        const auto event = QueueEventAfterWait11(g_to11.fence11.Get(), value);
        const bool held = !PollEvent11(event.Get(), 1000.0, nullptr);
        double latency = -1.0;
        Must(g_to11.fence12->Signal(value), "CPU fence Signal");
        const bool released = PollEvent11(event.Get(), 2000.0, &latency);
        record.detail = std::string(held ? "held for 1 s with D3D12 alive and silent" : "NOT held") + "; " +
                        (released ? "a CPU-side ID3D12Fence::Signal released it" : "the CPU signal did NOT release it");
        record.metrics = {{"release_latency_ms", latency}};
        Finish(record);
        if (!released) return;
    }
    Record record{"teardown", "d3d11_wait_pending_while_d3d12_objects_released", "info"};
    const uint64_t value = ++g_to11.value;
    const auto event = QueueEventAfterWait11(g_to11.fence11.Get(), value);
    const bool held_before = !PollEvent11(event.Get(), 200.0, nullptr);
    // Release every D3D12 object the probe holds; D3D11 keeps its own opened fence.
    WaitIdle12();
    g.graveyard.clear();
    g.pattern_psos.clear();
    g.timestamps12.Reset();
    g.timestamp_results12.Reset();
    g.evict12_a.Reset();
    g.evict12_b.Reset();
    g.decode_pso.Reset();
    g.reduce_pso.Reset();
    g.root_signature.Reset();
    g.srv_heap.Reset();
    g.rtv_heap.Reset();
    g.list12.Reset();
    for (auto& allocator : g.allocators) allocator.Reset();
    g.idle12.Reset();
    g_to12.fence12.Reset();
    g_to11.fence12.Reset();
    g_gate.fence12.Reset();
    g.queue12.Reset();
    g.info12.Reset();
    const ULONG remaining = g.device12.Reset();
    double latency = -1.0;
    const bool released = PollEvent11(event.Get(), 3000.0, &latency);
    record.detail = std::string(held_before ? "held before the release; " : "NOT held before the release; ") +
                    (released ? "released once D3D12's objects were gone"
                              : "STILL HELD 3 s after every D3D12 object was released: the D3D11 context is "
                                "stranded, so the bridge must CPU-signal its shared fences past every issued value "
                                "before releasing D3D12") +
                    "; d3d11 fence completed value " + Hex(g_to11.fence11->GetCompletedValue(), 16) +
                    ", d3d12 device references left " + std::to_string(remaining);
    record.metrics = {{"release_latency_ms", latency}};
    Finish(record);
}

// --- Ring: the bridge's protocol under deliberate skew ---------------------------------------------------------


// Three shared images, twelve frames, everything queued before any of it may run. One API is held back by a
// gate fence the CPU opens frame by frame, so the other runs ahead as far as the protocol lets it:
//   raw: D3D11's writes are gated; D3D12 must wait for each "written" value or it copies stale content.
//   war: D3D12's copies are gated; D3D11 must wait for each slot's "done" value before writing it again, or it
//        overwrites a frame D3D12 has not copied yet.
// Each runs with the full protocol (must be exact) and as a control with the guarding wait left out (must
// show the hazard, or the protocol run proves nothing).
struct RingConfig {
    const char* name;
    bool gate_d3d11;
    bool wait_written;
    bool wait_done;
    bool control;
    // Render into a private app image and copy it into the slot (copy at release), instead of rendering into
    // the slot (zero copy).
    bool copy_at_release;
};

void RingCase(Lineage lineage, const Size& size) {
    constexpr uint32_t kSlots = 3;
    constexpr uint32_t kFrames = 12;
    constexpr DXGI_FORMAT kView = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    TextureSpec spec;
    spec.width = size.width;
    spec.height = size.height;
    spec.format = ProtocolFormat(lineage);
    const char* format_name = lineage == Lineage::D3D12Plain ? "rgba8_srgb_typed" : "rgba8";
    const uint32_t bpp = 4;

    std::array<SharedTexture, kSlots> slots;
    std::array<ComPtr<ID3D11RenderTargetView>, kSlots> rtvs;
    for (uint32_t i = 0; i < kSlots; ++i) {
        slots[i] = CreateShared(lineage, spec);
        if (!slots[i].ok()) {
            Record record{"ring", CaseName(lineage, format_name, spec), "unsupported"};
            record.expect_debug_errors = true;
            record.detail = "create: " + slots[i].stage + " " + HrText(slots[i].hr);
            Finish(record);
            Settle();
            return;
        }
        Must(CreateRtv11(slots[i].texture11.Get(), kView, rtvs[i]), "ring RTV");
    }
    // Expected content of every frame, rendered on a private texture.
    std::vector<std::vector<uint8_t>> expected(kFrames);
    {
        const auto reference = CreateTexture11(
            Desc11(spec.width, spec.height, spec.format, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE));
        ComPtr<ID3D11RenderTargetView> reference_rtv;
        Must(CreateRtv11(reference.Get(), kView, reference_rtv), "reference RTV");
        for (uint32_t frame = 0; frame < kFrames; ++frame) {
            RenderPattern11(reference_rtv.Get(), spec.width, spec.height, 1000 + frame);
            expected[frame] = Read11(reference.Get(), 0);
        }
    }
    const auto stale = CreateTexture11(Desc11(spec.width, spec.height, spec.format, D3D11_BIND_SHADER_RESOURCE),
                                       PatternBytes(static_cast<std::size_t>(spec.width) * spec.height * bpp, 999,
                                                     spec.format));

    const RingConfig configs[] = {
        {"raw", true, true, true, false, false},
        {"raw_control_without_written_wait", true, false, true, true, false},
        {"war", false, true, true, false, false},
        {"war_control_without_done_wait", false, true, false, true, false},
        {"raw_copy_at_release", true, true, true, false, true},
        {"raw_copy_at_release_control_without_written_wait", true, false, true, true, true},
        {"war_copy_at_release", false, true, true, false, true},
        {"war_copy_at_release_control_without_done_wait", false, true, false, true, true},
    };
    // The app's own images for copy at release: a three-image private swapchain in DCS's format.
    std::array<ComPtr<ID3D11Texture2D>, kSlots> app_images;
    std::array<ComPtr<ID3D11RenderTargetView>, kSlots> app_rtvs;
    for (uint32_t i = 0; i < kSlots; ++i) {
        app_images[i] = CreateTexture11(Desc11(spec.width, spec.height, DXGI_FORMAT_R8G8B8A8_TYPELESS,
                                               D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE));
        Must(CreateRtv11(app_images[i].Get(), kView, app_rtvs[i]), "app RTV");
    }
    for (const RingConfig& config : configs) {
        Record record{"ring", CaseName(lineage, format_name, spec, config.name)};
        for (uint32_t i = 0; i < kSlots; ++i) {
            Access11 access(slots[i].mutex.Get());
            if (!access.ok()) Abort("AcquireSync " + HrText(access.result()));
            g.context11->CopyResource(slots[i].texture11.Get(), stale.Get());
        }
        Publish11To12();
        Settle();

        const uint64_t gate_base = g_gate.value;
        const uint64_t recycle_before = g.recycle_waits;
        std::array<uint64_t, kSlots> done_value{};
        std::vector<Readback12> readbacks(kFrames);
        double max_d3d11_wait_ms = 0.0;
        double max_d3d11_handoff_ms = 0.0;
        double max_d3d12_wait_ms = 0.0;
        const double submit_start = NowMs();
        for (uint32_t frame = 0; frame < kFrames; ++frame) {
            const uint32_t slot = frame % kSlots;
            if (config.gate_d3d11) Must(g.context11->Wait(g_gate.fence11.Get(), gate_base + frame + 1), "gate Wait");
            if (config.wait_done && done_value[slot] != 0) {
                const double t = NowMs();
                Must(g.context11->Wait(g_to11.fence11.Get(), done_value[slot]), "done Wait");
                max_d3d11_wait_ms = std::max(max_d3d11_wait_ms, NowMs() - t);
            }
            if (config.copy_at_release) {
                RenderPattern11(app_rtvs[frame % kSlots].Get(), spec.width, spec.height, 1000 + frame);
            }
            {
                Access11 access(slots[slot].mutex.Get());
                if (!access.ok()) Abort("AcquireSync " + HrText(access.result()));
                if (config.copy_at_release) {
                    g.context11->CopyResource(slots[slot].texture11.Get(), app_images[frame % kSlots].Get());
                } else {
                    RenderPattern11(rtvs[slot].Get(), spec.width, spec.height, 1000 + frame);
                }
                // The D3D11 side of the handoff, timed whole: ReleaseSync (keyed mutex only), Signal, Flush.
                const double t = NowMs();
                access.Release();
                Publish11To12(false);
                max_d3d11_handoff_ms = std::max(max_d3d11_handoff_ms, NowMs() - t);
            }
            if (config.wait_written) {
                const double t = NowMs();
                Must(g.queue12->Wait(g_to12.fence12.Get(), g_to12.value), "queue Wait");
                max_d3d12_wait_ms = std::max(max_d3d12_wait_ms, NowMs() - t);
            }
            if (!config.gate_d3d11) Must(g.queue12->Wait(g_gate.fence12.Get(), gate_base + frame + 1), "gate Wait");
            ID3D12GraphicsCommandList* list = Begin12();
            ID3D12Resource* texture = slots[slot].texture12.Get();
            Transition(list, texture, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
            readbacks[frame] = RecordReadback12(list, texture, 0);
            Transition(list, texture, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
            End12();
            done_value[slot] = Publish12To11(false);
        }
        const double submit_ms = NowMs() - submit_start;
        // Open the gate a frame at a time.
        for (uint32_t frame = 0; frame < kFrames; ++frame) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            Must(g_gate.fence12->Signal(gate_base + frame + 1), "gate Signal");
        }
        g_gate.value = gate_base + kFrames;
        // Later D3D11 work must not touch the slots before D3D12's last copies.
        for (uint32_t i = 0; i < kSlots; ++i) Must(g.context11->Wait(g_to11.fence11.Get(), done_value[i]), "Wait");
        WaitIdle12();
        WaitIdle11();
        uint32_t mismatched = 0;
        std::string first;
        for (uint32_t frame = 0; frame < kFrames; ++frame) {
            const Comparison comparison = CompareBytes(expected[frame], Unpack(readbacks[frame]));
            if (comparison.mismatches == 0) continue;
            if (mismatched++ == 0) {
                first = "frame " + std::to_string(frame) + ": " + DescribeMismatch(comparison, spec.width, bpp);
            }
        }
        const uint64_t recycle = g.recycle_waits - recycle_before;
        record.metrics = {{"mismatched_frames", mismatched},
                          {"submit_cpu_ms", submit_ms},
                          {"max_d3d11_wait_call_ms", max_d3d11_wait_ms},
                          {"max_d3d11_handoff_ms", max_d3d11_handoff_ms},
                          {"max_d3d12_wait_call_ms", max_d3d12_wait_ms},
                          {"cpu_recycle_waits", static_cast<double>(recycle)}};
        if (recycle != 0) {
            record.status = "fail";
            record.detail = "queuing needed CPU waits";
        } else if (config.control) {
            record.status = mismatched != 0 ? "pass" : "insensitive";
            record.detail = mismatched != 0 ? "hazard visible without the wait (" + first + ")"
                                            : "no hazard seen without the wait: the protocol run proves less";
        } else {
            record.status = mismatched == 0 ? "pass" : "fail";
            record.detail = mismatched == 0 ? "all frames exact" : first;
        }
        Finish(record);
        Settle();
    }
}

// --- Timings ---------------------------------------------------------------------------------------------------

// Each metric is a set of samples, summarised as median, min and max.
struct Stat {
    double median = std::numeric_limits<double>::quiet_NaN();
    double min = std::numeric_limits<double>::quiet_NaN();
    double max = std::numeric_limits<double>::quiet_NaN();
};

Stat Summarize(std::vector<double> samples) {
    samples.erase(std::remove_if(samples.begin(), samples.end(), [](double v) { return !std::isfinite(v); }),
                  samples.end());
    Stat stat;
    if (samples.empty()) return stat;
    std::sort(samples.begin(), samples.end());
    stat.median = samples[samples.size() / 2];
    stat.min = samples.front();
    stat.max = samples.back();
    return stat;
}

void AddStat(Record& record, const std::string& name, const Stat& stat) {
    record.metrics.push_back({name + "_ms", stat.median});
    record.metrics.push_back({name + "_min", stat.min});
    record.metrics.push_back({name + "_max", stat.max});
}

void CreateTimingResources() {
    if (g.evict11_a) return;
    constexpr UINT kEvictBytes = 256u << 20;
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = kEvictBytes;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    Must(g.device11->CreateBuffer(&desc, nullptr, &g.evict11_a), "evict buffer");
    Must(g.device11->CreateBuffer(&desc, nullptr, &g.evict11_b), "evict buffer");
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = kEvictBytes;
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    Must(g.device12->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COMMON,
                                             nullptr, IID_PPV_ARGS(&g.evict12_a)),
         "evict buffer");
    Must(g.device12->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COMMON,
                                             nullptr, IID_PPV_ARGS(&g.evict12_b)),
         "evict buffer");
    D3D12_QUERY_HEAP_DESC query{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 2, 0};
    Must(g.device12->CreateQueryHeap(&query, IID_PPV_ARGS(&g.timestamps12)), "CreateQueryHeap");
    D3D12_HEAP_PROPERTIES readback{};
    readback.Type = D3D12_HEAP_TYPE_READBACK;
    buffer.Width = sizeof(UINT64) * 2;
    Must(g.device12->CreateCommittedResource(&readback, D3D12_HEAP_FLAG_NONE, &buffer,
                                             D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                             IID_PPV_ARGS(&g.timestamp_results12)),
         "timestamp readback");
}

// The pattern drawn on D3D12, so private D3D12 baselines are in the state a game's rendering leaves them in.
ID3D12PipelineState* PatternPso12(DXGI_FORMAT format) {
    for (auto& [cached, pso] : g.pattern_psos) {
        if (cached == format) return pso.Get();
    }
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = g.root_signature.Get();
    desc.VS = {g.fullscreen_vs_code->GetBufferPointer(), g.fullscreen_vs_code->GetBufferSize()};
    desc.PS = {g.pattern_ps_code->GetBufferPointer(), g.pattern_ps_code->GetBufferSize()};
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.SampleMask = UINT_MAX;
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = format;
    desc.SampleDesc.Count = 1;
    ComPtr<ID3D12PipelineState> pso;
    Must(g.device12->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso)), "CreateGraphicsPipelineState");
    g.pattern_psos.push_back({format, pso});
    return pso.Get();
}

// Records a full-image smooth pattern draw into `texture` (which must be in RENDER_TARGET).
void RecordPattern12(ID3D12GraphicsCommandList* list, ID3D12Resource* texture, DXGI_FORMAT view,
                     D3D12_CPU_DESCRIPTOR_HANDLE rtv) {
    const D3D12_RESOURCE_DESC desc = texture->GetDesc();
    list->SetGraphicsRootSignature(g.root_signature.Get());
    list->SetPipelineState(PatternPso12(view));
    DecodeConstants constants;
    constants.mode = static_cast<uint32_t>(PatternMode::Smooth);
    list->SetGraphicsRoot32BitConstants(0, 8, &constants, 0);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const D3D12_VIEWPORT viewport{0, 0, static_cast<float>(desc.Width), static_cast<float>(desc.Height), 0, 1};
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(desc.Width), static_cast<LONG>(desc.Height)};
    list->RSSetViewports(1, &viewport);
    list->RSSetScissorRects(1, &scissor);
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    list->DrawInstanced(3, 1, 0, 0);
}

double ReadTimestamps11(ID3D11Query* disjoint_query, ID3D11Query* begin_query, ID3D11Query* end_query, int ops) {
    const double start = NowMs();
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
    UINT64 begin = 0;
    UINT64 end = 0;
    auto poll = [&](ID3D11Query* query, void* data, UINT size) {
        HRESULT hr = S_FALSE;
        while ((hr = g.context11->GetData(query, data, size, 0)) == S_FALSE) {
            if (NowMs() - start > 30000.0) Abort("timestamp query did not resolve in 30 s");
            std::this_thread::yield();
        }
        if (hr != S_OK) Abort("timestamp query failed " + HrText(hr));
    };
    poll(disjoint_query, &disjoint, sizeof(disjoint));
    poll(begin_query, &begin, sizeof(begin));
    poll(end_query, &end, sizeof(end));
    if (disjoint.Disjoint || disjoint.Frequency == 0 || end < begin) return std::numeric_limits<double>::quiet_NaN();
    return static_cast<double>(end - begin) * 1000.0 / static_cast<double>(disjoint.Frequency) / ops;
}

// One cold D3D11 sample: `prepare` (unmeasured), L2 evicted, the GPU idle, then `op` alone between timestamps.
double ColdSample11(const std::function<void()>& prepare, const std::function<void()>& op) {
    if (prepare) prepare();
    g.context11->CopyResource(g.evict11_b.Get(), g.evict11_a.Get());
    WaitIdle11();
    D3D11_QUERY_DESC disjoint_desc{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
    D3D11_QUERY_DESC stamp_desc{D3D11_QUERY_TIMESTAMP, 0};
    ComPtr<ID3D11Query> disjoint;
    ComPtr<ID3D11Query> begin;
    ComPtr<ID3D11Query> end;
    Must(g.device11->CreateQuery(&disjoint_desc, &disjoint), "disjoint query");
    Must(g.device11->CreateQuery(&stamp_desc, &begin), "timestamp query");
    Must(g.device11->CreateQuery(&stamp_desc, &end), "timestamp query");
    g.context11->Begin(disjoint.Get());
    g.context11->End(begin.Get());
    op();
    g.context11->End(end.Get());
    g.context11->End(disjoint.Get());
    return ReadTimestamps11(disjoint.Get(), begin.Get(), end.Get(), 1);
}

// One hot D3D11 sample: 16 back-to-back ops between timestamps, after 3 unmeasured ones (caches warm).
double HotSample11(const std::function<void()>& op) {
    constexpr int kOps = 16;
    for (int i = 0; i < 3; ++i) op();
    D3D11_QUERY_DESC disjoint_desc{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
    D3D11_QUERY_DESC stamp_desc{D3D11_QUERY_TIMESTAMP, 0};
    ComPtr<ID3D11Query> disjoint;
    ComPtr<ID3D11Query> begin;
    ComPtr<ID3D11Query> end;
    Must(g.device11->CreateQuery(&disjoint_desc, &disjoint), "disjoint query");
    Must(g.device11->CreateQuery(&stamp_desc, &begin), "timestamp query");
    Must(g.device11->CreateQuery(&stamp_desc, &end), "timestamp query");
    g.context11->Begin(disjoint.Get());
    g.context11->End(begin.Get());
    for (int i = 0; i < kOps; ++i) op();
    g.context11->End(end.Get());
    g.context11->End(disjoint.Get());
    return ReadTimestamps11(disjoint.Get(), begin.Get(), end.Get(), kOps);
}

double ReadTimestamps12(int ops) {
    UINT64 frequency = 0;
    Must(g.queue12->GetTimestampFrequency(&frequency), "GetTimestampFrequency");
    void* data = nullptr;
    D3D12_RANGE range{0, sizeof(UINT64) * 2};
    Must(g.timestamp_results12->Map(0, &range, &data), "timestamps Map");
    const UINT64 begin = static_cast<const UINT64*>(data)[0];
    const UINT64 end = static_cast<const UINT64*>(data)[1];
    D3D12_RANGE none{0, 0};
    g.timestamp_results12->Unmap(0, &none);
    if (end < begin || frequency == 0) return std::numeric_limits<double>::quiet_NaN();
    return static_cast<double>(end - begin) * 1000.0 / static_cast<double>(frequency) / ops;
}

// The same on D3D12. `ops` runs of `op` between the timestamps; cold samples evict L2 and idle the GPU first.
double Sample12(const std::function<void(ID3D12GraphicsCommandList*)>& prepare,
                const std::function<void(ID3D12GraphicsCommandList*)>& op, bool cold) {
    const int ops = cold ? 1 : 16;
    ID3D12GraphicsCommandList* list = Begin12();
    if (prepare) prepare(list);
    if (cold) {
        list->CopyBufferRegion(g.evict12_b.Get(), 0, g.evict12_a.Get(), 0, g.evict12_a->GetDesc().Width);
    } else {
        for (int i = 0; i < 3; ++i) op(list);
    }
    End12();
    WaitIdle12();
    list = Begin12();
    list->EndQuery(g.timestamps12.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
    for (int i = 0; i < ops; ++i) op(list);
    list->EndQuery(g.timestamps12.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
    list->ResolveQueryData(g.timestamps12.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, g.timestamp_results12.Get(), 0);
    End12();
    WaitIdle12();
    return ReadTimestamps12(ops);
}

// Hold the GPU busy for a while so clocks ramp before anything is timed.
void WarmUp(double milliseconds) {
    const auto target = CreateTexture11(Desc11(2048, 2048, DXGI_FORMAT_R16G16B16A16_FLOAT,
                                               D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE));
    ComPtr<ID3D11RenderTargetView> rtv;
    Must(CreateRtv11(target.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, rtv), "warm-up RTV");
    Constants11 constants;
    constants.iterations = 64;
    const double start = NowMs();
    while (NowMs() - start < milliseconds) {
        for (int i = 0; i < 8; ++i) Draw11(g.pattern_ps.Get(), rtv.Get(), nullptr, 0, 0, 2048, 2048, constants);
        WaitIdle11();
    }
}

struct TimedTarget {
    std::string name;
    ComPtr<ID3D11Texture2D> texture11;
    ComPtr<ID3D12Resource> texture12;
    ComPtr<IDXGIKeyedMutex> mutex;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11ShaderResourceView> srv;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv12{};
    bool d3d12_private = false;
    bool simultaneous = false;
    std::map<std::string, std::vector<double>> samples;
};

// GPU cost of each way a texture can be shared, against private textures, in both the typeless and the fully
// typed form (typelessness alone may cost compression, which must not be blamed on sharing). Two regimes:
//   cold: L2 evicted and the GPU idle before one op, as copy-at-release meets a frame the game rendered long
//         before it ends (the honest per-frame cost);
//   hot:  16 back-to-back ops with the data cache-resident (an upper bound on throughput).
// Targets are interleaved sample by sample so clock drift spreads evenly. A keyed mutex is held around each
// sample, outside the timestamps; its own cost is measured separately (KeyedMutexCost).
//   D3D11: copy-at-release from a just-rendered source (smooth and noise), draws, a clear, blending, and a
//          read-dominated reduce of the target;
//   D3D12: FG's capture per frame (COMMON -> COPY_SOURCE, a full-image CopyTextureRegion into a history texture
//          of the source's format, back to COMMON) and a read-dominated reduce. Shared targets are rendered by
//          D3D11; private D3D12 baselines are rendered by a D3D12 draw, as a D3D12 game's images would be.
void TimingCase(const char* name, DXGI_FORMAT typeless, DXGI_FORMAT typed, DXGI_FORMAT view, const Size& size) {
    constexpr int kColdSamples = 15;
    constexpr int kHotSamples = 7;
    CreateTimingResources();
    WarmUp(300.0);
    const uint32_t w = size.width;
    const uint32_t h = size.height;
    const float fw = static_cast<float>(w);
    const float fh = static_cast<float>(h);
    const UINT bind = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    std::vector<TimedTarget> targets;
    for (const DXGI_FORMAT format : {typeless, typed}) {
        TimedTarget target;
        target.name = format == typeless ? "private" : "private_typed";
        target.texture11 = CreateTexture11(Desc11(w, h, format, bind));
        targets.push_back(std::move(target));
    }
    struct SharedVariant {
        Lineage lineage;
        bool typed;
    };
    const SharedVariant variants[] = {
        {Lineage::D3D11KeyedMutex, false},  {Lineage::D3D11Shared, false}, {Lineage::D3D11Shared, true},
        {Lineage::D3D12Simultaneous, false}, {Lineage::D3D12Simultaneous, true}, {Lineage::D3D12Plain, true},
    };
    for (const SharedVariant& variant : variants) {
        TextureSpec spec;
        spec.width = w;
        spec.height = h;
        spec.format = variant.typed ? typed : typeless;
        SharedTexture shared = CreateShared(variant.lineage, spec);
        if (!shared.ok()) continue;
        TimedTarget target;
        target.name = std::string(LineageName(variant.lineage)) + (variant.typed ? "_typed" : "");
        target.texture11 = shared.texture11;
        target.texture12 = shared.texture12;
        target.mutex = shared.mutex;
        target.simultaneous =
            (shared.texture12->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS) != 0;
        targets.push_back(std::move(target));
    }
    for (TimedTarget& target : targets) {
        Must(CreateRtv11(target.texture11.Get(), view, target.rtv), "timing RTV");
        Must(CreateSrv11(target.texture11.Get(), view, target.srv), "timing SRV");
    }
    // Private D3D12 baselines for the D3D12 metrics, rendered on D3D12.
    std::vector<TimedTarget> targets12;
    for (TimedTarget& target : targets) {
        if (!target.texture12) continue;
        TimedTarget copy;
        copy.name = target.name;
        copy.texture11 = target.texture11;
        copy.texture12 = target.texture12;
        copy.mutex = target.mutex;
        copy.rtv = target.rtv;
        copy.simultaneous = target.simultaneous;
        targets12.push_back(std::move(copy));
    }
    for (const DXGI_FORMAT format : {typeless, typed}) {
        TimedTarget target;
        target.name = format == typeless ? "private" : "private_typed";
        target.texture12 = CreatePrivate12(w, h, format, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,
                                           D3D12_RESOURCE_STATE_COMMON);
        target.rtv12 = Rtv12(target.texture12.Get(), view);
        target.d3d12_private = true;
        targets12.push_back(std::move(target));
    }

    // Copy sources as a game leaves its images: just rendered (so possibly compressed), smooth or noise.
    const auto source = CreateTexture11(Desc11(w, h, typeless, bind));
    ComPtr<ID3D11RenderTargetView> source_rtv;
    Must(CreateRtv11(source.Get(), view, source_rtv), "RTV");
    Constants11 smooth;
    smooth.mode = static_cast<uint32_t>(PatternMode::Smooth);
    const Constants11 noise;
    const auto render_source = [&](const Constants11& constants) {
        return [&, constants] { Draw11(g.pattern_ps.Get(), source_rtv.Get(), nullptr, 0, 0, fw, fh, constants); };
    };
    // Reduce outputs: a quarter of each side, so the source read dominates.
    const uint32_t rw = (w + 3) / 4;
    const uint32_t rh = (h + 3) / 4;
    const auto reduced11 =
        CreateTexture11(Desc11(rw, rh, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D11_BIND_UNORDERED_ACCESS));
    ComPtr<ID3D11UnorderedAccessView> reduced11_uav;
    Must(CreateUav11(reduced11.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, reduced11_uav), "reduce UAV");
    const auto reduced12 = CreatePrivate12(rw, rh, DXGI_FORMAT_R16G16B16A16_FLOAT,
                                           D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                           D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    // D3D11 metrics: each a (prepare, op) pair for a target.
    struct Metric11 {
        const char* name;
        std::function<std::function<void()>(TimedTarget&)> prepare;
        std::function<std::function<void()>(TimedTarget&)> op;
        bool hot;
    };
    const auto none = [](TimedTarget&) { return std::function<void()>(); };
    const std::vector<Metric11> metrics11 = {
        {"copy_smooth", [&](TimedTarget&) { return std::function<void()>(render_source(smooth)); },
         [&](TimedTarget& t) { return std::function<void()>([&] { g.context11->CopyResource(t.texture11.Get(), source.Get()); }); },
         true},
        {"copy_noise", [&](TimedTarget&) { return std::function<void()>(render_source(noise)); },
         [&](TimedTarget& t) { return std::function<void()>([&] { g.context11->CopyResource(t.texture11.Get(), source.Get()); }); },
         true},
        {"draw_smooth", none,
         [&](TimedTarget& t) {
             return std::function<void()>(
                 [&] { Draw11(g.pattern_ps.Get(), t.rtv.Get(), nullptr, 0, 0, fw, fh, smooth); });
         },
         true},
        {"draw_noise", none,
         [&](TimedTarget& t) {
             return std::function<void()>(
                 [&] { Draw11(g.pattern_ps.Get(), t.rtv.Get(), nullptr, 0, 0, fw, fh, noise); });
         },
         false},
        {"clear", none,
         [&](TimedTarget& t) {
             return std::function<void()>([&] { g.context11->ClearRenderTargetView(t.rtv.Get(), kClearA); });
         },
         false},
        {"blend4_smooth", none,
         [&](TimedTarget& t) {
             return std::function<void()>([&] {
                 for (int i = 0; i < 4; ++i) {
                     Draw11(g.pattern_ps.Get(), t.rtv.Get(), nullptr, 0, 0, fw, fh, smooth, true);
                 }
             });
         },
         false},
        {"read_reduce",
         [&](TimedTarget& t) {
             return std::function<void()>(
                 [&] { Draw11(g.pattern_ps.Get(), t.rtv.Get(), nullptr, 0, 0, fw, fh, smooth); });
         },
         [&](TimedTarget& t) {
             return std::function<void()>(
                 [&] { Dispatch11(g.reduce_cs.Get(), t.srv.Get(), reduced11_uav.Get(), rw, rh); });
         },
         true},
    };
    for (const Metric11& metric : metrics11) {
        for (int sample = 0; sample < kColdSamples; ++sample) {
            for (TimedTarget& target : targets) {
                Access11 access(target.mutex.Get());
                if (!access.ok()) Abort("AcquireSync in timing");
                target.samples[std::string(metric.name) + "_cold"].push_back(
                    ColdSample11(metric.prepare(target), metric.op(target)));
            }
        }
        if (!metric.hot) continue;
        for (int sample = 0; sample < kHotSamples; ++sample) {
            for (TimedTarget& target : targets) {
                Access11 access(target.mutex.Get());
                if (!access.ok()) Abort("AcquireSync in timing");
                const auto prepare = metric.prepare(target);
                if (prepare) prepare();
                target.samples[std::string(metric.name) + "_hot"].push_back(HotSample11(metric.op(target)));
            }
        }
    }

    // D3D12 metrics. A shared target is rendered by D3D11 and handed over before each sample, and handed back
    // after; a private D3D12 target is rendered by D3D12 in the sample's unmeasured part.
    auto prepare12 = [&](TimedTarget& target) {
        if (target.d3d12_private) {
            return std::function<void(ID3D12GraphicsCommandList*)>([&](ID3D12GraphicsCommandList* list) {
                Transition(list, target.texture12.Get(), D3D12_RESOURCE_STATE_COMMON,
                           D3D12_RESOURCE_STATE_RENDER_TARGET);
                RecordPattern12(list, target.texture12.Get(), view, target.rtv12);
                Transition(list, target.texture12.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET,
                           D3D12_RESOURCE_STATE_COMMON);
            });
        }
        {
            Access11 access(target.mutex.Get());
            if (!access.ok()) Abort("AcquireSync in timing");
            Draw11(g.pattern_ps.Get(), target.rtv.Get(), nullptr, 0, 0, fw, fh, smooth);
        }
        Publish11To12();
        return std::function<void(ID3D12GraphicsCommandList*)>();
    };
    const auto history = CreatePrivate12(w, h, typeless, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    const auto history_typed = CreatePrivate12(w, h, typed, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    auto capture12 = [&](TimedTarget& target) {
        ID3D12Resource* destination = target.texture12->GetDesc().Format == typed ? history_typed.Get() : history.Get();
        return [&target, destination, w, h](ID3D12GraphicsCommandList* list) {
            Transition(list, target.texture12.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION to{};
            to.pResource = destination;
            to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            D3D12_TEXTURE_COPY_LOCATION from{};
            from.pResource = target.texture12.Get();
            from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            const D3D12_BOX box{0, 0, 0, w, h, 1};
            list->CopyTextureRegion(&to, 0, 0, 0, &from, &box);
            Transition(list, target.texture12.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
        };
    };
    std::map<std::string, D3D12_GPU_DESCRIPTOR_HANDLE> tables;
    for (TimedTarget& target : targets12) {
        tables[target.name] = DecodeTable12(target.texture12.Get(), view, reduced12.Get());
    }
    auto reduce12 = [&](TimedTarget& target) {
        const D3D12_GPU_DESCRIPTOR_HANDLE table = tables[target.name];
        return [&target, table, &reduced12, rw, rh](ID3D12GraphicsCommandList* list) {
            Transition(list, target.texture12.Get(), D3D12_RESOURCE_STATE_COMMON,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            ID3D12DescriptorHeap* heaps[] = {g.srv_heap.Get()};
            list->SetDescriptorHeaps(1, heaps);
            list->SetComputeRootSignature(g.root_signature.Get());
            list->SetPipelineState(g.reduce_pso.Get());
            DecodeConstants constants;
            list->SetComputeRoot32BitConstants(0, 8, &constants, 0);
            list->SetComputeRootDescriptorTable(1, table);
            list->Dispatch((rw + 7) / 8, (rh + 7) / 8, 1);
            UavBarrier(list, reduced12.Get());
            Transition(list, target.texture12.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_COMMON);
        };
    };
    struct Metric12 {
        const char* name;
        std::function<std::function<void(ID3D12GraphicsCommandList*)>(TimedTarget&)> op;
    };
    const std::vector<Metric12> metrics12 = {{"capture", capture12}, {"read_reduce", reduce12}};
    for (const Metric12& metric : metrics12) {
        for (const bool cold : {true, false}) {
            const int samples = cold ? kColdSamples : kHotSamples;
            for (int sample = 0; sample < samples; ++sample) {
                for (TimedTarget& target : targets12) {
                    const auto prepare = prepare12(target);
                    target.samples[std::string(metric.name) + (cold ? "_cold" : "_hot")].push_back(
                        Sample12(prepare, metric.op(target), cold));
                    if (!target.d3d12_private) Publish12To11();
                }
            }
        }
    }

    const std::string suffix = std::string(name) + "/" + FormatName(view) + "/" + SizeText(size);
    const double bytes = static_cast<double>(w) * h * BytesPerPixel(typeless);
    std::map<std::string, double> signal;
    for (TimedTarget& target : targets) {
        Record record{"timing_d3d11", target.name + "/" + suffix, "info"};
        for (const auto& [metric, values] : target.samples) AddStat(record, metric, Summarize(values));
        const Stat copy_noise = Summarize(target.samples["copy_noise_cold"]);
        const Stat copy_smooth = Summarize(target.samples["copy_smooth_cold"]);
        record.metrics.push_back({"copy_noise_cold_GBps", 2.0 * bytes / (copy_noise.median * 1.0e6)});
        signal[target.name] = copy_noise.median / copy_smooth.median;
        record.metrics.push_back({"compression_signal", signal[target.name]});
        Finish(record);
    }
    for (TimedTarget& target : targets12) {
        Record record{"timing_d3d12", target.name + (target.d3d12_private ? "_rendered_on_d3d12" : "") + "/" + suffix,
                      "info"};
        for (const auto& [metric, values] : target.samples) AddStat(record, metric, Summarize(values));
        record.metrics.push_back({"simultaneous_access", target.simultaneous ? 1.0 : 0.0});
        Finish(record);
    }
    {
        // Whether this case can show compression at all: a private copy of incompressible noise must cost
        // clearly more than one of a smooth image, or "shared costs the same as private" means nothing.
        Record record{"timing_sensitivity", suffix, "info"};
        const double private_signal = signal["private"];
        record.metrics = {{"private_compression_signal", private_signal},
                          {"private_typed_compression_signal", signal["private_typed"]}};
        record.detail = private_signal >= 1.15
                            ? "sensitive: compression is visible on private targets"
                            : "INSENSITIVE: private copies show no compression benefit, so shared-vs-private "
                              "equality says nothing here";
        if (private_signal < 1.15) record.status = "insensitive_timing";
        Finish(record);
    }
    Settle();
}

// What the keyed mutex costs per handoff: on the CPU (AcquireSync + ReleaseSync, idle and with real work queued
// in front of the release) and on the GPU (32 one-pixel clears with and without the mutex around each).
void KeyedMutexCost() {
    Record record{"timing_keyed_mutex", "handoff", "info"};
    TextureSpec spec;
    spec.width = 64;
    spec.height = 64;
    spec.format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
    const SharedTexture shared = CreateShared(Lineage::D3D11KeyedMutex, spec);
    if (!shared.ok()) {
        record.status = "unsupported";
        Finish(record);
        return;
    }
    ComPtr<ID3D11RenderTargetView> rtv;
    Must(CreateRtv11(shared.texture11.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, rtv), "RTV");
    const auto busy = CreateTexture11(Desc11(1024, 1024, DXGI_FORMAT_R16G16B16A16_FLOAT,
                                             D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE));
    ComPtr<ID3D11RenderTargetView> busy_rtv;
    Must(CreateRtv11(busy.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, busy_rtv), "RTV");
    std::vector<double> idle_us;
    std::vector<double> release_with_work_us;
    for (int i = 0; i < 300; ++i) {
        double start = NowMs();
        Must(shared.mutex->AcquireSync(0, kMutexTimeoutMs), "AcquireSync");
        Must(shared.mutex->ReleaseSync(0), "ReleaseSync");
        idle_us.push_back((NowMs() - start) * 1000.0);
        Must(shared.mutex->AcquireSync(0, kMutexTimeoutMs), "AcquireSync");
        Constants11 constants;
        for (int d = 0; d < 50; ++d) Draw11(g.pattern_ps.Get(), busy_rtv.Get(), nullptr, 0, 0, 64, 64, constants);
        start = NowMs();
        Must(shared.mutex->ReleaseSync(0), "ReleaseSync");
        release_with_work_us.push_back((NowMs() - start) * 1000.0);
    }
    WaitIdle11();
    constexpr int kHandoffs = 32;
    const D3D11_RECT pixel{0, 0, 1, 1};
    auto clears = [&](bool with_mutex) {
        std::vector<double> samples;
        for (int run = 0; run < 9; ++run) {
            D3D11_QUERY_DESC disjoint_desc{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
            D3D11_QUERY_DESC stamp_desc{D3D11_QUERY_TIMESTAMP, 0};
            ComPtr<ID3D11Query> disjoint;
            ComPtr<ID3D11Query> begin;
            ComPtr<ID3D11Query> end;
            Must(g.device11->CreateQuery(&disjoint_desc, &disjoint), "query");
            Must(g.device11->CreateQuery(&stamp_desc, &begin), "query");
            Must(g.device11->CreateQuery(&stamp_desc, &end), "query");
            if (!with_mutex) Must(shared.mutex->AcquireSync(0, kMutexTimeoutMs), "AcquireSync");
            g.context11->Begin(disjoint.Get());
            g.context11->End(begin.Get());
            for (int i = 0; i < kHandoffs; ++i) {
                if (with_mutex) Must(shared.mutex->AcquireSync(0, kMutexTimeoutMs), "AcquireSync");
                g.context11->ClearView(rtv.Get(), kClearA, &pixel, 1);
                if (with_mutex) Must(shared.mutex->ReleaseSync(0), "ReleaseSync");
            }
            g.context11->End(end.Get());
            g.context11->End(disjoint.Get());
            if (!with_mutex) Must(shared.mutex->ReleaseSync(0), "ReleaseSync");
            samples.push_back(ReadTimestamps11(disjoint.Get(), begin.Get(), end.Get(), kHandoffs) * 1000.0);
        }
        return Median(samples);
    };
    const double plain_us = clears(false);
    const double mutex_us = clears(true);
    record.metrics = {{"cpu_acquire_release_idle_median_us", Median(idle_us)},
                      {"cpu_acquire_release_idle_p99_us", Percentile(idle_us, 0.99)},
                      {"cpu_release_with_50_draws_queued_median_us", Median(release_with_work_us)},
                      {"cpu_release_with_50_draws_queued_p99_us", Percentile(release_with_work_us, 0.99)},
                      {"gpu_per_handoff_us", mutex_us - plain_us},
                      {"gpu_pixel_clear_without_mutex_us", plain_us}};
    Finish(record);
    Settle();
}

// CPU cost of the fence calls on the app's thread, idle and with real work queued in front of the Flush (the
// release path's situation: the Flush submits the frame's pending work).
void CpuCosts() {
    Record record{"timing_cpu", "sync_calls", "info"};
    constexpr int kRuns = 300;
    const auto busy = CreateTexture11(Desc11(1024, 1024, DXGI_FORMAT_R16G16B16A16_FLOAT,
                                             D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE));
    ComPtr<ID3D11RenderTargetView> busy_rtv;
    Must(CreateRtv11(busy.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, busy_rtv), "RTV");
    std::vector<double> signal_flush_us;
    std::vector<double> signal_flush_busy_us;
    std::vector<double> context_wait_us;
    std::vector<double> queue_wait_us;
    std::vector<double> queue_signal_us;
    for (int i = 0; i < kRuns; ++i) {
        double start = NowMs();
        Publish11To12(false);
        signal_flush_us.push_back((NowMs() - start) * 1000.0);
        Constants11 constants;
        for (int d = 0; d < 200; ++d) Draw11(g.pattern_ps.Get(), busy_rtv.Get(), nullptr, 0, 0, 64, 64, constants);
        start = NowMs();
        Publish11To12(false);
        signal_flush_busy_us.push_back((NowMs() - start) * 1000.0);
        start = NowMs();
        Must(g.queue12->Wait(g_to12.fence12.Get(), g_to12.value), "queue Wait");
        queue_wait_us.push_back((NowMs() - start) * 1000.0);
        start = NowMs();
        Publish12To11(false);
        queue_signal_us.push_back((NowMs() - start) * 1000.0);
        start = NowMs();
        Must(g.context11->Wait(g_to11.fence11.Get(), g_to11.value), "context Wait");
        context_wait_us.push_back((NowMs() - start) * 1000.0);
        if (i % 50 == 49) WaitIdle11();
    }
    Settle();
    record.metrics = {{"d3d11_signal_flush_idle_median_us", Median(signal_flush_us)},
                      {"d3d11_signal_flush_idle_p99_us", Percentile(signal_flush_us, 0.99)},
                      {"d3d11_signal_flush_200_draws_queued_median_us", Median(signal_flush_busy_us)},
                      {"d3d11_signal_flush_200_draws_queued_p99_us", Percentile(signal_flush_busy_us, 0.99)},
                      {"d3d11_wait_median_us", Median(context_wait_us)},
                      {"d3d11_wait_p99_us", Percentile(context_wait_us, 0.99)},
                      {"d3d12_queue_wait_median_us", Median(queue_wait_us)},
                      {"d3d12_queue_signal_median_us", Median(queue_signal_us)}};
    Finish(record);
}

void Timings() {
    if (g_options.debug) {
        Record record{"timing", "skipped", "info"};
        record.detail = "timings need the debug layers off";
        Finish(record);
        return;
    }
    WarmUp(1500.0);
    CpuCosts();
    KeyedMutexCost();
    for (const Size& size : {kPeripheral, kFocus, kCanvas}) {
        TimingCase("rgba8", DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
                   DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, size);
        TimingCase("rgba16f", DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R16G16B16A16_FLOAT,
                   DXGI_FORMAT_R16G16B16A16_FLOAT, size);
    }
}

// --- Output ----------------------------------------------------------------------------------------------------

void WriteJson(const std::string& path) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        std::cerr << "could not write " << path << '\n';
        return;
    }
    char adapter_name[256]{};
    WideCharToMultiByte(CP_UTF8, 0, g.adapter_desc.Description, -1, adapter_name, sizeof(adapter_name), nullptr,
                        nullptr);
    out << "{\"tool\":\"depthxr_bridge_probe\",\"schema\":1,\"debug\":" << (g_options.debug ? "true" : "false")
        << ",\"gpu_validation\":" << (g_options.gpu_validation ? "true" : "false")
        << ",\"adapter\":\"" << JsonEscape(adapter_name) << "\",\"records\":[";
    for (std::size_t i = 0; i < g_records.size(); ++i) {
        const Record& record = g_records[i];
        out << (i ? ",\n" : "\n") << "{\"section\":\"" << JsonEscape(record.section) << "\",\"name\":\""
            << JsonEscape(record.name) << "\",\"status\":\"" << record.status << "\",\"detail\":\""
            << JsonEscape(record.detail) << "\",\"metrics\":{";
        for (std::size_t m = 0; m < record.metrics.size(); ++m) {
            const double value = record.metrics[m].second;
            out << (m ? "," : "") << "\"" << JsonEscape(record.metrics[m].first) << "\":";
            if (std::isfinite(value)) {
                out << value;
            } else {
                out << "null";
            }
        }
        out << "},\"debug_message_count\":" << record.debug_message_count
            << ",\"debug_error\":" << (record.debug_error ? "true" : "false") << ",\"debug_messages\":[";
        for (std::size_t m = 0; m < record.debug_messages.size(); ++m) {
            out << (m ? "," : "") << "\"" << JsonEscape(record.debug_messages[m]) << "\"";
        }
        out << "]}";
    }
    out << "\n]}\n";
}

bool IsFailure(const Record& record) {
    if (record.status == "fail" || record.status == "insensitive") return true;
    // insensitive_timing (a timing case that cannot show compression) is reported, not failed.
    return record.debug_error && !record.expect_debug_errors;
}

void PrintUsage() {
    std::cout << "depthxr_bridge_probe [--debug] [--gpu-validation] [--timings] [--quick]\n"
                 "                     [--feature-level-11-0] [--single-threaded] [--removal] [--teardown]\n"
                 "                     [--json <path>]\n"
                 "Direct3D 11 <-> 12 sharing probe for the D3D11 bridge (real GPU only).\n";
}

} // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument = argv[i];
        if (argument == "--debug") {
            g_options.debug = true;
        } else if (argument == "--gpu-validation") {
            g_options.debug = g_options.gpu_validation = true;
        } else if (argument == "--timings") {
            g_options.timings = true;
        } else if (argument == "--quick") {
            g_options.quick = true;
        } else if (argument == "--feature-level-11-0") {
            g_options.feature_level_11_0 = true;
        } else if (argument == "--single-threaded") {
            g_options.single_threaded = true;
        } else if (argument == "--removal") {
            g_options.removal = true;
        } else if (argument == "--teardown") {
            g_options.teardown = true;
        } else if (argument == "--json" && i + 1 < argc) {
            g_options.json_path = argv[++i];
        } else {
            PrintUsage();
            return argument == "--help" || argument == "-h" ? 0 : 64;
        }
    }

    CreateDevices();
    CreatePipelines();
    CreateFences();
    if (g_options.removal) {
        RemovalCase();
        if (!g_options.json_path.empty()) WriteJson(g_options.json_path);
        return 0;
    }
    if (g_options.teardown) {
        TeardownCase();
        if (!g_options.json_path.empty()) WriteJson(g_options.json_path);
        // The D3D11 context may be stranded behind a wait nothing will satisfy: leave without touching it.
        std::cout.flush();
        TerminateProcess(GetCurrentProcess(), 0);
    }
    Capabilities();
    SelfTest();
    Settle();

    const std::vector<Size> sizes = g_options.quick ? std::vector<Size>{kQuick} : std::vector<Size>{kPeripheral, kFocus};
    const Size shape_size = g_options.quick ? kQuick : kFocus;
    for (const Lineage lineage : kLineages) {
        for (const ColorFormat& format : ColorFormats()) {
            for (const Size& size : sizes) ColorCase(lineage, format, size);
        }
        if (!g_options.quick) {
            // DCS's own format at the unrounded size and at the compositor canvas size.
            ColorCase(lineage, ColorFormats()[0], kUnrounded);
            ColorCase(lineage, ColorFormats()[0], kCanvas);
        }
    }
    for (const Lineage lineage : kLineages) {
        for (const DepthFormat& format : DepthFormats()) {
            DepthDirectCase(lineage, format, shape_size);
            DepthCopyCase(lineage, format, shape_size);
        }
    }
    for (const Lineage lineage : kLineages) ShapeCases(lineage, shape_size);
    for (const Lineage lineage : kLineages) {
        CopyAtReleaseCase(lineage, shape_size);
        if (!g_options.quick) CopyAtReleaseCase(lineage, kCanvas);
    }
    CreationMatrix(kQuick);
    DebugLayerControl();
    for (const Lineage lineage : kLineages) RestingStateCase(lineage, shape_size);
    UnacquiredWriteCase(shape_size);
    if (g_options.gpu_validation) {
        Record record{"fences", "skipped_under_gpu_validation", "info"};
        record.detail = "synchronized queue validation would hold the cross-API waits on the CPU";
        Finish(record);
    } else {
        FenceCases();
        for (const Lineage lineage : kLineages) RingCase(lineage, shape_size);
    }
    if (g_options.timings) Timings();

    std::size_t failures = 0;
    std::cout << "\n=== Summary ===\n";
    for (const Record& record : g_records) {
        if (!IsFailure(record)) continue;
        ++failures;
        std::cout << "FAILED: " << record.section << " " << record.name << " [" << record.status << "] "
                  << record.detail << (record.debug_error ? " (debug-layer error)" : "") << '\n';
    }
    std::size_t expected_failures = 0;
    std::size_t unexpected_passes = 0;
    std::size_t unsupported = 0;
    for (const Record& record : g_records) {
        expected_failures += record.status == "expected_fail";
        unexpected_passes += record.status == "unexpected_pass";
        if (record.status == "unsupported") {
            ++unsupported;
            std::cout << "unsupported: " << record.section << " " << record.name << " " << record.detail << '\n';
        }
    }
    std::cout << g_records.size() << " records, " << failures << " failure(s), " << unsupported
              << " unsupported, " << expected_failures << " expected failure(s), " << unexpected_passes
              << " unexpected pass(es)\n";
    if (!g_options.json_path.empty()) WriteJson(g_options.json_path);
    return failures == 0 ? 0 : 1;
}
