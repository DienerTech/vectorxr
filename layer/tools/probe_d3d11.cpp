// vectorxr_hardware_probe --graphics d3d11 (the default): the line scene, the pattern and the GPU load through
// Direct3D 11.
#include "probe_common.h"

#include <d3d11_1.h>
#include <d3dcompiler.h>
#include <dxgi1_2.h>
#include <DirectXMath.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>

using Microsoft::WRL::ComPtr;
using namespace DirectX;

namespace probe {
namespace {

bool IsTypelessFormat(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16_TYPELESS:
    case DXGI_FORMAT_R32_TYPELESS:
        return true;
    default:
        return false;
    }
}

DXGI_FORMAT ResolveRenderTargetFormat(DXGI_FORMAT texture_format,
                                      int64_t requested_format) {
    if (!IsTypelessFormat(texture_format)) {
        return texture_format;
    }
    const DXGI_FORMAT requested = static_cast<DXGI_FORMAT>(requested_format);
    if (requested != DXGI_FORMAT_UNKNOWN && !IsTypelessFormat(requested)) {
        return requested;
    }
    switch (texture_format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
        return DXGI_FORMAT_B8G8R8X8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R16G16_TYPELESS:
        return DXGI_FORMAT_R16G16_UNORM;
    case DXGI_FORMAT_R32_TYPELESS:
        return DXGI_FORMAT_R32_FLOAT;
    default:
        return texture_format;
    }
}

void CreateSwapchainRenderTarget(ID3D11Device* device,
                                 ID3D11Texture2D* texture,
                                 int64_t requested_format,
                                 ID3D11RenderTargetView** render_target) {
    D3D11_TEXTURE2D_DESC texture_desc{};
    texture->GetDesc(&texture_desc);

    D3D11_RENDER_TARGET_VIEW_DESC view_desc{};
    view_desc.Format = ResolveRenderTargetFormat(
        texture_desc.Format, requested_format);
    if (texture_desc.SampleDesc.Count > 1 && texture_desc.ArraySize > 1) {
        view_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY;
        view_desc.Texture2DMSArray.FirstArraySlice = 0;
        view_desc.Texture2DMSArray.ArraySize = 1;
    } else if (texture_desc.SampleDesc.Count > 1) {
        view_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMS;
    } else if (texture_desc.ArraySize > 1) {
        view_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
        view_desc.Texture2DArray.MipSlice = 0;
        view_desc.Texture2DArray.FirstArraySlice = 0;
        view_desc.Texture2DArray.ArraySize = 1;
    } else {
        view_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        view_desc.Texture2D.MipSlice = 0;
    }

    const HRESULT result = device->CreateRenderTargetView(
        texture, &view_desc, render_target);
    if (FAILED(result)) {
        std::ostringstream stream;
        stream << "CreateRenderTargetView failed with HRESULT 0x" << std::hex
               << static_cast<uint32_t>(result) << std::dec
               << "; requestedFormat=" << requested_format
               << ", texture={size=" << texture_desc.Width << "x" << texture_desc.Height
               << ", mips=" << texture_desc.MipLevels
               << ", array=" << texture_desc.ArraySize
               << ", format=" << static_cast<uint32_t>(texture_desc.Format)
               << ", samples=" << texture_desc.SampleDesc.Count
               << ", bindFlags=0x" << std::hex << texture_desc.BindFlags << std::dec
               << "}, viewFormat=" << static_cast<uint32_t>(view_desc.Format)
               << ", viewDimension=" << static_cast<uint32_t>(view_desc.ViewDimension);
        throw std::runtime_error(stream.str());
    }
}

// --render-check: D3D11CreateDevice's results that mean the machine lacks what the check needs (the Graphics Tools'
// SDK layers for --d3d11-debug, a hardware adapter), not that the probe is wrong.
bool DeviceUnavailable(HRESULT result) {
    return result == DXGI_ERROR_SDK_COMPONENT_MISSING || result == DXGI_ERROR_UNSUPPORTED;
}

D3D11_RECT ToD3D11(const PixelRect& rect, int32_t x, int32_t y) {
    return {rect.left + x, rect.top + y, rect.right + x, rect.bottom + y};
}

struct Vertex {
    XMFLOAT3 position;
    XMFLOAT3 color;
};

struct SceneConstants {
    XMFLOAT4X4 view_projection;
};

void CompileShader(const char* source,
                   const char* entry,
                   const char* profile,
                   ID3DBlob** output) {
    ComPtr<ID3DBlob> errors;
    const HRESULT result = D3DCompile(
        source, std::strlen(source), "pivot_space_probe.hlsl", nullptr, nullptr,
        entry, profile, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, output, &errors);
    if (FAILED(result)) {
        const std::string detail = errors
            ? std::string(static_cast<const char*>(errors->GetBufferPointer()),
                          errors->GetBufferSize())
            : std::string{};
        throw std::runtime_error("D3DCompile failed for " + std::string(entry) +
                                 ": " + detail);
    }
}

// The asymmetric line scene: a ground grid, colored axes and two differently placed beacons.
class DiagnosticRenderer {
public:
    void Initialize(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11DeviceContext1* context1) {
        device_ = device;
        context_ = context;
        context1_ = context1;

        static constexpr char kShaderSource[] = R"(
cbuffer SceneConstants : register(b0) {
    row_major float4x4 viewProjection;
};

struct VertexInput {
    float3 position : POSITION;
    float3 color : COLOR0;
};

struct VertexOutput {
    float4 position : SV_POSITION;
    float3 color : COLOR0;
};

VertexOutput VSMain(VertexInput input) {
    VertexOutput output;
    output.position = mul(float4(input.position, 1.0), viewProjection);
    output.color = input.color;
    return output;
}

float4 PSMain(VertexOutput input) : SV_TARGET {
    return float4(input.color, 1.0);
}
)";

        ComPtr<ID3DBlob> vertex_blob;
        ComPtr<ID3DBlob> pixel_blob;
        CompileShader(kShaderSource, "VSMain", "vs_5_0", &vertex_blob);
        CompileShader(kShaderSource, "PSMain", "ps_5_0", &pixel_blob);
        ThrowIfFailed(device_->CreateVertexShader(
                          vertex_blob->GetBufferPointer(), vertex_blob->GetBufferSize(),
                          nullptr, &vertex_shader_),
                      "CreateVertexShader");
        ThrowIfFailed(device_->CreatePixelShader(
                          pixel_blob->GetBufferPointer(), pixel_blob->GetBufferSize(),
                          nullptr, &pixel_shader_),
                      "CreatePixelShader");

        const D3D11_INPUT_ELEMENT_DESC elements[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,
             static_cast<UINT>(offsetof(Vertex, position)),
             D3D11_INPUT_PER_VERTEX_DATA, 0},
            {"COLOR", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,
             static_cast<UINT>(offsetof(Vertex, color)),
             D3D11_INPUT_PER_VERTEX_DATA, 0},
        };
        ThrowIfFailed(device_->CreateInputLayout(
                          elements, static_cast<UINT>(std::size(elements)),
                          vertex_blob->GetBufferPointer(), vertex_blob->GetBufferSize(),
                          &input_layout_),
                      "CreateInputLayout");

        const std::vector<Vertex> vertices = BuildScene();
        vertex_count_ = static_cast<UINT>(vertices.size());
        D3D11_BUFFER_DESC vertex_desc{};
        vertex_desc.ByteWidth = static_cast<UINT>(vertices.size() * sizeof(Vertex));
        vertex_desc.Usage = D3D11_USAGE_IMMUTABLE;
        vertex_desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        D3D11_SUBRESOURCE_DATA vertex_data{};
        vertex_data.pSysMem = vertices.data();
        ThrowIfFailed(device_->CreateBuffer(&vertex_desc, &vertex_data, &vertex_buffer_),
                      "CreateBuffer(vertex)");

        D3D11_BUFFER_DESC constant_desc{};
        constant_desc.ByteWidth = sizeof(SceneConstants);
        constant_desc.Usage = D3D11_USAGE_DYNAMIC;
        constant_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        constant_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        ThrowIfFailed(device_->CreateBuffer(&constant_desc, nullptr, &constant_buffer_),
                      "CreateBuffer(constants)");

        D3D11_DEPTH_STENCIL_DESC depth_state_desc{};
        depth_state_desc.DepthEnable = TRUE;
        depth_state_desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        depth_state_desc.DepthFunc = D3D11_COMPARISON_LESS;
        ThrowIfFailed(device_->CreateDepthStencilState(&depth_state_desc, &depth_state_),
                      "CreateDepthStencilState");
    }

    // The scene from `view_pose` into `rect` of a `width` x `height` image. A rect smaller than the image (one half
    // of a side-by-side image) clears only its own pixels; `clear_depth` clears all of the depth buffer.
    void Render(ID3D11RenderTargetView* render_target,
                ID3D11DepthStencilView* depth_target,
                const PixelRect& rect,
                uint32_t width,
                uint32_t height,
                const XrPosef& view_pose,
                const XrFovf& fov,
                uint32_t eye,
                bool clear_depth) {
        const std::array<float, 4> clear_color = ClearColor(eye);
        if (rect.left == 0 && rect.top == 0 && rect.right == static_cast<int32_t>(width) &&
            rect.bottom == static_cast<int32_t>(height)) {
            context_->ClearRenderTargetView(render_target, clear_color.data());
        } else {
            const D3D11_RECT clear_rect = ToD3D11(rect, 0, 0);
            context1_->ClearView(render_target, clear_color.data(), &clear_rect, 1);
        }
        if (clear_depth) {
            context_->ClearDepthStencilView(depth_target, D3D11_CLEAR_DEPTH, 1.0f, 0);
        }

        const XMVECTOR orientation = XMVectorSet(
            view_pose.orientation.x, view_pose.orientation.y,
            view_pose.orientation.z, view_pose.orientation.w);
        const XMMATRIX world_from_view =
            XMMatrixRotationQuaternion(orientation) *
            XMMatrixTranslation(view_pose.position.x,
                                view_pose.position.y,
                                view_pose.position.z);
        const XMMATRIX view_from_world = XMMatrixInverse(nullptr, world_from_view);
        const float left = std::tan(fov.angleLeft) * kNearZ;
        const float right = std::tan(fov.angleRight) * kNearZ;
        const float bottom = std::tan(fov.angleDown) * kNearZ;
        const float top = std::tan(fov.angleUp) * kNearZ;
        const XMMATRIX projection = XMMatrixPerspectiveOffCenterRH(
            left, right, bottom, top, kNearZ, kFarZ);

        SceneConstants constants{};
        XMStoreFloat4x4(&constants.view_projection, view_from_world * projection);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        ThrowIfFailed(context_->Map(constant_buffer_.Get(), 0,
                                    D3D11_MAP_WRITE_DISCARD, 0, &mapped),
                      "Map(constants)");
        std::memcpy(mapped.pData, &constants, sizeof(constants));
        context_->Unmap(constant_buffer_.Get(), 0);

        D3D11_VIEWPORT viewport{};
        viewport.TopLeftX = static_cast<float>(rect.left);
        viewport.TopLeftY = static_cast<float>(rect.top);
        viewport.Width = static_cast<float>(rect.right - rect.left);
        viewport.Height = static_cast<float>(rect.bottom - rect.top);
        viewport.MinDepth = 0.0f;
        viewport.MaxDepth = 1.0f;
        context_->RSSetViewports(1, &viewport);
        context_->OMSetRenderTargets(1, &render_target, depth_target);
        context_->OMSetDepthStencilState(depth_state_.Get(), 0);
        context_->IASetInputLayout(input_layout_.Get());
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
        const UINT stride = sizeof(Vertex);
        const UINT offset = 0;
        ID3D11Buffer* vertex_buffer = vertex_buffer_.Get();
        context_->IASetVertexBuffers(0, 1, &vertex_buffer, &stride, &offset);
        context_->VSSetShader(vertex_shader_.Get(), nullptr, 0);
        ID3D11Buffer* constant_buffer = constant_buffer_.Get();
        context_->VSSetConstantBuffers(0, 1, &constant_buffer);
        context_->PSSetShader(pixel_shader_.Get(), nullptr, 0);
        context_->Draw(vertex_count_, 0);
    }

private:
    static std::vector<Vertex> BuildScene() {
        std::vector<Vertex> vertices;
        const auto line = [&](XMFLOAT3 start, XMFLOAT3 end, XMFLOAT3 color) {
            vertices.push_back({start, color});
            vertices.push_back({end, color});
        };

        const XMFLOAT3 grid{0.18f, 0.22f, 0.27f};
        for (int x = -10; x <= 10; ++x) {
            line({static_cast<float>(x), -1.5f, -1.0f},
                 {static_cast<float>(x), -1.5f, -21.0f}, grid);
        }
        for (int z = 1; z <= 21; ++z) {
            line({-10.0f, -1.5f, -static_cast<float>(z)},
                 {10.0f, -1.5f, -static_cast<float>(z)}, grid);
        }

        const XMFLOAT3 origin{0.0f, -1.5f, -4.0f};
        line(origin, {2.5f, -1.5f, -4.0f}, {1.0f, 0.1f, 0.1f});
        line(origin, {0.0f, 1.0f, -4.0f}, {0.1f, 1.0f, 0.1f});
        line(origin, {0.0f, -1.5f, -7.0f}, {0.1f, 0.35f, 1.0f});

        const auto beacon = [&](float x, float z, XMFLOAT3 color) {
            line({x, -1.5f, z}, {x, 2.5f, z}, color);
            line({x - 0.4f, 2.1f, z}, {x + 0.4f, 2.1f, z}, color);
            line({x, 1.7f, z - 0.4f}, {x, 1.7f, z + 0.4f}, color);
        };
        beacon(-3.0f, -7.0f, {1.0f, 0.85f, 0.1f});
        beacon(4.0f, -12.0f, {1.0f, 0.1f, 0.85f});
        return vertices;
    }

    ID3D11Device* device_{nullptr};
    ID3D11DeviceContext* context_{nullptr};
    ID3D11DeviceContext1* context1_{nullptr};
    ComPtr<ID3D11VertexShader> vertex_shader_;
    ComPtr<ID3D11PixelShader> pixel_shader_;
    ComPtr<ID3D11InputLayout> input_layout_;
    ComPtr<ID3D11Buffer> vertex_buffer_;
    ComPtr<ID3D11Buffer> constant_buffer_;
    ComPtr<ID3D11DepthStencilState> depth_state_;
    UINT vertex_count_{0};
};

// The GPU load's compute shader, timestamp queries and event queries on Direct3D 11.
class D3D11GpuLoadBackend final : public GpuLoadBackend {
public:
    D3D11GpuLoadBackend(ID3D11Device* device, ID3D11DeviceContext* context) : device_(device), context_(context) {
        if (device_->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_0) {
            throw std::runtime_error("--gpu-ms needs a Direct3D 11.0 GPU (compute shaders)");
        }
        static constexpr char kShaderSource[] = R"(
cbuffer LoadConstants : register(b0) {
    uint iterations;
};

RWStructuredBuffer<float> values : register(u0);

[numthreads(64, 1, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    float v = values[id.x];
    // The values carry over from dispatch to dispatch: start again long before they could overflow.
    if (!(abs(v) < 1.0e4)) {
        v = 1.0 + id.x * 1.0e-6;
    }
    [loop]
    for (uint i = 0; i < iterations; ++i) {
        v = mad(v, 1.000001, sin(v) * 1.0e-3);
    }
    values[id.x] = v;
}
)";
        ComPtr<ID3DBlob> blob;
        CompileShader(kShaderSource, "CSMain", "cs_5_0", &blob);
        ThrowIfFailed(device_->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr,
                                                   &shader_),
                      "CreateComputeShader(GPU load)");

        std::vector<float> seeds(kLoadThreads);
        for (uint32_t i = 0; i < kLoadThreads; ++i) {
            seeds[i] = 1.0f + static_cast<float>(i) * 1.0e-6f;
        }
        D3D11_BUFFER_DESC values_desc{};
        values_desc.ByteWidth = kLoadThreads * sizeof(float);
        values_desc.Usage = D3D11_USAGE_DEFAULT;
        values_desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        values_desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        values_desc.StructureByteStride = sizeof(float);
        D3D11_SUBRESOURCE_DATA values_data{};
        values_data.pSysMem = seeds.data();
        ThrowIfFailed(device_->CreateBuffer(&values_desc, &values_data, &values_), "CreateBuffer(GPU load)");
        D3D11_UNORDERED_ACCESS_VIEW_DESC view_desc{};
        view_desc.Format = DXGI_FORMAT_UNKNOWN;
        view_desc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
        view_desc.Buffer.NumElements = kLoadThreads;
        ThrowIfFailed(device_->CreateUnorderedAccessView(values_.Get(), &view_desc, &values_view_),
                      "CreateUnorderedAccessView(GPU load)");

        D3D11_BUFFER_DESC constant_desc{};
        constant_desc.ByteWidth = 16;
        constant_desc.Usage = D3D11_USAGE_DYNAMIC;
        constant_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        constant_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        ThrowIfFailed(device_->CreateBuffer(&constant_desc, nullptr, &constants_),
                      "CreateBuffer(GPU load constants)");

        for (Timer& timer : calibration_.views) {
            timer = CreateTimer();
        }
        for (Frame& frame : ring_) {
            for (Timer& timer : frame.views) {
                timer = CreateTimer();
            }
            frame.done = CreateQuery(D3D11_QUERY_EVENT);
        }
    }

    void Warm(uint32_t iterations, int count) override {
        for (int i = 0; i < count; ++i) {
            Dispatch(iterations);
        }
    }

    std::optional<double> TimeViews(uint32_t views, uint32_t iterations, uint32_t chunks) override {
        double ms = 0.0;
        for (uint32_t view = 0; view < views && view < kMaximumViews; ++view) {
            RecordTimed(calibration_.views[view], iterations, chunks);
        }
        for (uint32_t view = 0; view < views && view < kMaximumViews; ++view) {
            const std::optional<double> view_ms = Read(calibration_.views[view]);
            if (!view_ms) {
                return std::nullopt;
            }
            ms += *view_ms;
        }
        return ms;
    }

    void BeginSlot(std::size_t) override {}

    void RecordTimed(std::size_t slot, uint32_t view, uint32_t iterations, uint32_t chunks) override {
        RecordTimed(ring_[slot].views[view], iterations, chunks);
    }

    void SignalFrame(std::size_t slot) override {
        context_->End(ring_[slot].done.Get());
    }

    bool FrameDone(std::size_t slot, bool flush) override {
        return Poll(ring_[slot].done.Get(), nullptr, 0, flush);
    }

    void WaitFrame(std::size_t slot) override {
        while (!Poll(ring_[slot].done.Get(), nullptr, 0, false)) {
            std::this_thread::yield();
        }
    }

    std::optional<double> ReadView(std::size_t slot, uint32_t view) override {
        return Read(ring_[slot].views[view]);
    }

private:
    struct Timer {
        ComPtr<ID3D11Query> disjoint;
        ComPtr<ID3D11Query> start;
        ComPtr<ID3D11Query> end;
    };

    struct Frame {
        std::array<Timer, kMaximumViews> views;
        ComPtr<ID3D11Query> done; // event: the frame's GPU work has run
    };

    ComPtr<ID3D11Query> CreateQuery(D3D11_QUERY type) {
        D3D11_QUERY_DESC desc{};
        desc.Query = type;
        ComPtr<ID3D11Query> query;
        ThrowIfFailed(device_->CreateQuery(&desc, &query), "CreateQuery(GPU load)");
        return query;
    }

    Timer CreateTimer() {
        return {CreateQuery(D3D11_QUERY_TIMESTAMP_DISJOINT), CreateQuery(D3D11_QUERY_TIMESTAMP),
                CreateQuery(D3D11_QUERY_TIMESTAMP)};
    }

    // `count` dispatches of `iterations` each (--gpu-chunks), one after another.
    void Dispatch(uint32_t iterations, uint32_t count = 1) {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        ThrowIfFailed(context_->Map(constants_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped),
                      "Map(GPU load constants)");
        const std::array<uint32_t, 4> constants{iterations, 0, 0, 0};
        std::memcpy(mapped.pData, constants.data(), sizeof(constants));
        context_->Unmap(constants_.Get(), 0);
        ID3D11Buffer* constant_buffer = constants_.Get();
        ID3D11UnorderedAccessView* view = values_view_.Get();
        context_->CSSetShader(shader_.Get(), nullptr, 0);
        context_->CSSetConstantBuffers(0, 1, &constant_buffer);
        context_->CSSetUnorderedAccessViews(0, 1, &view, nullptr);
        for (uint32_t i = 0; i < count; ++i) {
            context_->Dispatch(kLoadGroups, 1, 1);
        }
        // No compute state is left behind for the frame's rendering or a layer's work.
        ID3D11UnorderedAccessView* no_view = nullptr;
        ID3D11Buffer* no_buffer = nullptr;
        context_->CSSetUnorderedAccessViews(0, 1, &no_view, nullptr);
        context_->CSSetConstantBuffers(0, 1, &no_buffer);
        context_->CSSetShader(nullptr, nullptr, 0);
    }

    // Whether `query` has its result (written to `data` when given). Only the first poll of a wait flushes.
    bool Poll(ID3D11Query* query, void* data, UINT size, bool flush) {
        const HRESULT result =
            context_->GetData(query, data, size, flush ? 0 : D3D11_ASYNC_GETDATA_DONOTFLUSH);
        ThrowIfFailed(result, "ID3D11DeviceContext::GetData(GPU load)");
        return result == S_OK;
    }

    void Wait(ID3D11Query* query, void* data, UINT size) {
        if (Poll(query, data, size, true)) {
            return;
        }
        while (!Poll(query, data, size, false)) {
            std::this_thread::yield();
        }
    }

    // A timer's span in ms; nullopt when it straddled a clock change.
    std::optional<double> Read(const Timer& timer) {
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
        UINT64 start = 0;
        UINT64 end = 0;
        Wait(timer.disjoint.Get(), &disjoint, sizeof(disjoint));
        Wait(timer.start.Get(), &start, sizeof(start));
        Wait(timer.end.Get(), &end, sizeof(end));
        if (disjoint.Disjoint || disjoint.Frequency == 0 || end < start) {
            return std::nullopt;
        }
        return static_cast<double>(end - start) * 1000.0 / static_cast<double>(disjoint.Frequency);
    }

    void RecordTimed(const Timer& timer, uint32_t iterations, uint32_t chunks) {
        context_->Begin(timer.disjoint.Get());
        context_->End(timer.start.Get());
        Dispatch(iterations, chunks);
        context_->End(timer.end.Get());
        context_->End(timer.disjoint.Get());
    }

    ID3D11Device* device_{nullptr};
    ID3D11DeviceContext* context_{nullptr};
    ComPtr<ID3D11ComputeShader> shader_;
    ComPtr<ID3D11Buffer> values_;
    ComPtr<ID3D11UnorderedAccessView> values_view_;
    ComPtr<ID3D11Buffer> constants_;
    Frame calibration_; // its views' timers only
    std::array<Frame, kRing> ring_;
};

// The Direct3D 11 debug layer's errors and warnings over the run (the first
// few of them, and how many); returns how many.
uint64_t ReportD3D11Messages(ID3D11Device* device) {
    ComPtr<ID3D11InfoQueue> queue;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&queue)))) {
        std::cout << "Direct3D 11 debug layer: unavailable\n";
        return 0;
    }
    uint64_t errors = 0;
    uint64_t warnings = 0;
    int shown = 0;
    for (UINT64 i = 0; i < queue->GetNumStoredMessages(); ++i) {
        SIZE_T size = 0;
        queue->GetMessage(i, nullptr, &size);
        std::vector<char> storage(size);
        auto* message = reinterpret_cast<D3D11_MESSAGE*>(storage.data());
        if (FAILED(queue->GetMessage(i, message, &size))) continue;
        if (message->Severity <= D3D11_MESSAGE_SEVERITY_ERROR) {
            ++errors;
        } else if (message->Severity == D3D11_MESSAGE_SEVERITY_WARNING) {
            ++warnings;
        } else {
            continue;
        }
        if (shown++ < 10) {
            std::cout << "Direct3D 11 debug layer [" << message->ID << "]: " << message->pDescription << "\n";
        }
    }
    std::cout << "Direct3D 11 debug layer: " << errors << " errors, " << warnings << " warnings (of "
              << queue->GetNumStoredMessages() << " stored, " << queue->GetNumMessagesDiscardedByMessageCountLimit()
              << " discarded)\n";
    return errors + warnings;
}

class D3D11Graphics final : public ProbeGraphics {
public:
    explicit D3D11Graphics(const Options& options) : options_(options) {}

    std::string Describe() const override {
        return "Direct3D 11";
    }

    const char* InstanceExtension() const override {
        return XR_KHR_D3D11_ENABLE_EXTENSION_NAME;
    }

    void CreateDevice(XrInstance instance, XrSystemId system) override {
        PFN_xrGetD3D11GraphicsRequirementsKHR get_requirements = nullptr;
        CheckXr(instance, xrGetInstanceProcAddr(
                              instance, "xrGetD3D11GraphicsRequirementsKHR",
                              reinterpret_cast<PFN_xrVoidFunction*>(&get_requirements)),
                "xrGetInstanceProcAddr(xrGetD3D11GraphicsRequirementsKHR)");
        XrGraphicsRequirementsD3D11KHR requirements{
            XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
        CheckXr(instance, get_requirements(instance, system, &requirements),
                "xrGetD3D11GraphicsRequirementsKHR");

        ComPtr<IDXGIFactory1> factory;
        ThrowIfFailed(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),
                      "CreateDXGIFactory1");
        ComPtr<IDXGIAdapter1> selected_adapter;
        for (UINT index = 0;; ++index) {
            ComPtr<IDXGIAdapter1> candidate;
            if (factory->EnumAdapters1(index, &candidate) == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            DXGI_ADAPTER_DESC1 description{};
            ThrowIfFailed(candidate->GetDesc1(&description), "IDXGIAdapter1::GetDesc1");
            if (description.AdapterLuid.HighPart == requirements.adapterLuid.HighPart &&
                description.AdapterLuid.LowPart == requirements.adapterLuid.LowPart) {
                selected_adapter = candidate;
                std::wcout << L"Graphics adapter: " << description.Description << L"\n";
                break;
            }
        }
        if (!selected_adapter) {
            throw std::runtime_error("could not find the D3D11 adapter requested by the runtime");
        }

        const std::array<D3D_FEATURE_LEVEL, 7> feature_levels{
            D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0,
            D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
            D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
            D3D_FEATURE_LEVEL_9_3,
        };
        D3D_FEATURE_LEVEL created_level{};
        ThrowIfFailed(D3D11CreateDevice(
                          selected_adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                          D3D11_CREATE_DEVICE_BGRA_SUPPORT |
                              (options_.d3d11_debug ? D3D11_CREATE_DEVICE_DEBUG : 0),
                          feature_levels.data(), static_cast<UINT>(feature_levels.size()),
                          D3D11_SDK_VERSION, &device_, &created_level, &context_),
                      "D3D11CreateDevice");
        if (created_level < requirements.minFeatureLevel) {
            throw std::runtime_error("created D3D11 device does not meet the runtime feature-level requirement");
        }
        DeviceCreated();
    }

    // --render-check: the default hardware adapter, without OpenXR.
    void CreateStandaloneDevice() {
        const std::array<D3D_FEATURE_LEVEL, 4> feature_levels{
            D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
        };
        const HRESULT result = D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
            D3D11_CREATE_DEVICE_BGRA_SUPPORT | (options_.d3d11_debug ? D3D11_CREATE_DEVICE_DEBUG : 0),
            feature_levels.data(), static_cast<UINT>(feature_levels.size()), D3D11_SDK_VERSION, &device_, nullptr,
            &context_);
        if (DeviceUnavailable(result)) {
            std::ostringstream message;
            message << "D3D11CreateDevice failed with HRESULT 0x" << std::hex << static_cast<uint32_t>(result)
                    << (options_.d3d11_debug ? " (no hardware Direct3D 11 adapter, or the Graphics Tools' SDK layers "
                                               "for --d3d11-debug are not installed)"
                                             : " (no hardware Direct3D 11 adapter)");
            throw Unavailable(message.str());
        }
        ThrowIfFailed(result, "D3D11CreateDevice");
        DeviceCreated();
    }

    ID3D11Device* Device() const {
        return device_.Get();
    }

    ID3D11DeviceContext* Context() const {
        return context_.Get();
    }

    const void* Binding() const override {
        return &binding_;
    }

    std::vector<int64_t> ColorFormats() const override {
        return {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, DXGI_FORMAT_R8G8B8A8_UNORM,
                DXGI_FORMAT_B8G8R8A8_UNORM};
    }

    int64_t DepthFormat() const override {
        return DXGI_FORMAT_D32_FLOAT;
    }

    std::unique_ptr<GpuLoad> CreateGpuLoad(const Options& options) override {
        return std::make_unique<GpuLoad>(std::make_unique<D3D11GpuLoadBackend>(device_.Get(), context_.Get()),
                                         options);
    }

    uint32_t AddSwapchain(XrInstance instance, XrSwapchain swapchain, const XrSwapchainCreateInfo& info,
                          SwapchainRole role) override {
        uint32_t image_count = 0;
        CheckXr(instance, xrEnumerateSwapchainImages(swapchain, 0, &image_count, nullptr),
                "xrEnumerateSwapchainImages(count)");
        std::vector<XrSwapchainImageD3D11KHR> images(image_count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
        CheckXr(instance,
                xrEnumerateSwapchainImages(swapchain, image_count, &image_count,
                                           reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())),
                "xrEnumerateSwapchainImages");
        std::vector<uint64_t> textures;
        for (const XrSwapchainImageD3D11KHR& image : images) {
            textures.push_back(reinterpret_cast<uint64_t>(image.texture));
        }
        return AddImages(textures, info, role);
    }

    uint32_t AddImages(const std::vector<uint64_t>& images, const XrSwapchainCreateInfo& info,
                       SwapchainRole role) override {
        ImageSet set;
        set.width = info.width;
        set.height = info.height;
        for (const uint64_t image : images) {
            auto* texture = reinterpret_cast<ID3D11Texture2D*>(image);
            if (role == SwapchainRole::Depth) {
                D3D11_DEPTH_STENCIL_VIEW_DESC view{};
                view.Format = DXGI_FORMAT_D32_FLOAT;
                view.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
                ComPtr<ID3D11DepthStencilView> target;
                ThrowIfFailed(device_->CreateDepthStencilView(texture, &view, &target),
                              "CreateDepthStencilView(depth swapchain)");
                set.depth_targets.push_back(target);
            } else {
                ComPtr<ID3D11RenderTargetView> target;
                CreateSwapchainRenderTarget(device_.Get(), texture, info.format, &target);
                set.render_targets.push_back(target);
            }
        }
        if (role == SwapchainRole::Projection) {
            D3D11_TEXTURE2D_DESC depth_desc{};
            depth_desc.Width = info.width;
            depth_desc.Height = info.height;
            depth_desc.MipLevels = 1;
            depth_desc.ArraySize = 1;
            depth_desc.Format = DXGI_FORMAT_D32_FLOAT;
            depth_desc.SampleDesc.Count = 1;
            depth_desc.Usage = D3D11_USAGE_DEFAULT;
            depth_desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
            ThrowIfFailed(device_->CreateTexture2D(&depth_desc, nullptr, &set.depth_texture),
                          "CreateTexture2D(depth)");
            ThrowIfFailed(device_->CreateDepthStencilView(set.depth_texture.Get(), nullptr, &set.depth_target),
                          "CreateDepthStencilView");
        }
        set.image_count = static_cast<uint32_t>(images.size());
        sets_.push_back(std::move(set));
        return static_cast<uint32_t>(sets_.size() - 1);
    }

    uint32_t ImageCount(uint32_t id) const override {
        return sets_[id].image_count;
    }

    // Direct3D 11 keeps in-flight resources alive by reference counting: no GPU wait.
    void RemoveSwapchains() override {
        sets_.clear();
    }

    void BeginFrame() override {}

    void Draw(const ViewDraw& draw) override {
        const ImageSet& color = sets_[draw.color];
        ID3D11RenderTargetView* target = color.render_targets[draw.color_image].Get();
        if (draw.quad) {
            context_->ClearRenderTargetView(target, kQuadBackground.data());
            DrawPattern(target, draw.rect, PatternKind::Quad);
            return;
        }
        ID3D11DepthStencilView* depth_target =
            draw.depth ? sets_[*draw.depth].depth_targets[draw.depth_image].Get() : color.depth_target.Get();
        renderer_.Render(target, depth_target, draw.rect, color.width, color.height, draw.pose, draw.fov,
                         draw.view % 2, draw.first_in_image);
        if (draw.pattern) {
            DrawPattern(target, draw.rect, draw.view >= 2 ? PatternKind::Focus : PatternKind::Peripheral);
        }
    }

    // The release flushes: nothing to submit.
    void ImageDone() override {}
    void EndFrame() override {}

    uint64_t Report() override {
        return options_.d3d11_debug ? ReportD3D11Messages(device_.Get()) : 0;
    }

private:
    void DeviceCreated() {
        ThrowIfFailed(context_.As(&context1_), "ID3D11DeviceContext1");
        renderer_.Initialize(device_.Get(), context_.Get(), context1_.Get());
        binding_.device = device_.Get();
    }

    struct ImageSet {
        uint32_t width{0};
        uint32_t height{0};
        uint32_t image_count{0};
        std::vector<ComPtr<ID3D11RenderTargetView>> render_targets;
        std::vector<ComPtr<ID3D11DepthStencilView>> depth_targets; // a depth swapchain's
        ComPtr<ID3D11Texture2D> depth_texture;                     // Projection: private depth
        ComPtr<ID3D11DepthStencilView> depth_target;
    };

    // The pattern in `rect` (its own pixels), with ClearView's rects.
    void DrawPattern(ID3D11RenderTargetView* target, const PixelRect& rect, PatternKind kind) {
        const double seconds =
            std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        const Pattern pattern = PatternRects(static_cast<uint32_t>(rect.right - rect.left),
                                             static_cast<uint32_t>(rect.bottom - rect.top), kind, seconds);
        std::vector<D3D11_RECT> squares;
        for (const PixelRect& square : pattern.grid) {
            squares.push_back(ToD3D11(square, rect.left, rect.top));
        }
        context1_->ClearView(target, pattern.grid_color.data(), squares.data(), static_cast<UINT>(squares.size()));
        const D3D11_RECT bar = ToD3D11(pattern.bar, rect.left, rect.top);
        context1_->ClearView(target, pattern.bar_color.data(), &bar, 1);
    }

    Options options_;
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<ID3D11DeviceContext1> context1_;
    XrGraphicsBindingD3D11KHR binding_{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    DiagnosticRenderer renderer_;
    std::vector<ImageSet> sets_;
};

// --render-check on Direct3D 11: textures as a runtime would make them, read back through staging copies.
class D3D11CheckImages final : public CheckImages {
public:
    explicit D3D11CheckImages(const Options& options) : graphics_(options) {
        graphics_.CreateStandaloneDevice();
    }

    ProbeGraphics& Graphics() override {
        return graphics_;
    }

    uint32_t Add(const XrSwapchainCreateInfo& info, SwapchainRole role, uint32_t count) override {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = info.width;
        desc.Height = info.height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = static_cast<DXGI_FORMAT>(info.format);
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = role == SwapchainRole::Depth ? D3D11_BIND_DEPTH_STENCIL
                                                      : D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        std::vector<ComPtr<ID3D11Texture2D>> textures(count);
        std::vector<uint64_t> handles;
        for (ComPtr<ID3D11Texture2D>& texture : textures) {
            ThrowIfFailed(graphics_.Device()->CreateTexture2D(&desc, nullptr, &texture), "CreateTexture2D(check)");
            handles.push_back(reinterpret_cast<uint64_t>(texture.Get()));
        }
        const uint32_t id = graphics_.AddImages(handles, info, role);
        if (sets_.size() <= id) {
            sets_.resize(id + 1);
        }
        sets_[id] = std::move(textures);
        return id;
    }

    std::vector<uint8_t> ColorPixels(uint32_t id, uint32_t image) override {
        ID3D11Texture2D* texture = sets_[id][image].Get();
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        ThrowIfFailed(graphics_.Device()->CreateTexture2D(&desc, nullptr, &staging), "CreateTexture2D(staging)");
        graphics_.Context()->CopyResource(staging.Get(), texture);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        ThrowIfFailed(graphics_.Context()->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Map(staging)");
        std::vector<uint8_t> pixels(static_cast<std::size_t>(desc.Width) * desc.Height * 4);
        for (UINT row = 0; row < desc.Height; ++row) {
            std::memcpy(pixels.data() + static_cast<std::size_t>(row) * desc.Width * 4,
                        static_cast<const uint8_t*>(mapped.pData) + static_cast<std::size_t>(row) * mapped.RowPitch,
                        static_cast<std::size_t>(desc.Width) * 4);
        }
        graphics_.Context()->Unmap(staging.Get(), 0);
        return pixels;
    }

    // The Direct3D 11 pattern is ClearView, which cannot write depth.
    std::optional<std::vector<float>> DepthPixels(uint32_t, uint32_t) override {
        return std::nullopt;
    }

    void Remove() override {
        graphics_.RemoveSwapchains();
        sets_.clear();
    }

private:
    D3D11Graphics graphics_;
    std::vector<std::vector<ComPtr<ID3D11Texture2D>>> sets_;
};

} // namespace

std::unique_ptr<CheckImages> CreateD3D11CheckImages(const Options& options) {
    return std::make_unique<D3D11CheckImages>(options);
}

std::unique_ptr<ProbeGraphics> CreateD3D11Graphics(const Options& options) {
    return std::make_unique<D3D11Graphics>(options);
}

bool RunD3D11GpuCalibration(const Options& options) {
    const std::array<D3D_FEATURE_LEVEL, 4> feature_levels{
        D3D_FEATURE_LEVEL_12_1, D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
    };
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ThrowIfFailed(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                    options.d3d11_debug ? D3D11_CREATE_DEVICE_DEBUG : 0, feature_levels.data(),
                                    static_cast<UINT>(feature_levels.size()), D3D11_SDK_VERSION, &device, nullptr,
                                    &context),
                  "D3D11CreateDevice");
    ComPtr<IDXGIDevice> dxgi_device;
    ComPtr<IDXGIAdapter> adapter;
    DXGI_ADAPTER_DESC description{};
    if (SUCCEEDED(device.As(&dxgi_device)) && SUCCEEDED(dxgi_device->GetAdapter(&adapter)) &&
        SUCCEEDED(adapter->GetDesc(&description))) {
        std::wcout << L"Graphics adapter: " << description.Description << L"\n";
    }

    bool within = false;
    {
        GpuLoad load(std::make_unique<D3D11GpuLoadBackend>(device.Get(), context.Get()), options);
        within = RunGpuCalibrationLoop(load, options, [] {}, [&] { context->Flush(); }); // as a frame's release would
    }
    if (options.d3d11_debug) {
        ReportD3D11Messages(device.Get());
    }
    return within;
}

void RunD3D11SelfTest() {
    Expect(ResolveRenderTargetFormat(DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) ==
               DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
           "typeless swapchain RTV format resolution");
    Expect(ResolveRenderTargetFormat(DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM) ==
               DXGI_FORMAT_B8G8R8A8_UNORM,
           "typed swapchain RTV format preservation");
    const D3D11Graphics graphics(Options{});
    Expect(graphics.ColorFormats().front() == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB &&
               ChooseFormat({DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB}, graphics.ColorFormats()) ==
                   DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
           "Direct3D 11 colour formats prefer sRGB");
    Expect(DeviceUnavailable(DXGI_ERROR_SDK_COMPONENT_MISSING) && DeviceUnavailable(DXGI_ERROR_UNSUPPORTED) &&
               !DeviceUnavailable(E_INVALIDARG) && !DeviceUnavailable(E_OUTOFMEMORY) && !DeviceUnavailable(S_OK),
           "--render-check skips without the SDK layers or a hardware adapter, and fails otherwise");
}

} // namespace probe
