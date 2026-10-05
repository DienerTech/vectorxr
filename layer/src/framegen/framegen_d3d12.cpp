#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <vulkan/vulkan_core.h>
#include <openxr/openxr_platform.h>

#include "depthxr/framegen_gpu.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <unordered_map>

namespace depthxr {
namespace {
using Microsoft::WRL::ComPtr;

constexpr std::size_t kFramesInFlight = 3;
constexpr UINT kDescriptorsPerFrame = 1024;
constexpr UINT kCaptureSlots = 8;
// Depth captures run outside a frame's descriptor range: one dispatch per
// capture slot, after every frame's range.
constexpr UINT kDescriptorsPerDispatch = 6;

std::string Failure(const char* operation, HRESULT hr) {
    return std::string(operation) + " failed: hr=" + std::to_string(static_cast<long>(hr));
}

DXGI_FORMAT ViewFormat(DXGI_FORMAT native, std::int64_t requested) {
    switch (native) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return static_cast<DXGI_FORMAT>(requested);
    default:
        return native;
    }
}

struct Texture {
    ComPtr<ID3D12Resource> resource;
    DXGI_FORMAT view_format{DXGI_FORMAT_UNKNOWN};
    D3D12_RESOURCE_STATES state{D3D12_RESOURCE_STATE_COMMON};
    bool uav{false};
};

// Identifies the image a view's history was captured from at release time.
struct CaptureKey {
    bool valid{false};
    XrSwapchain swapchain{XR_NULL_HANDLE};
    std::uint32_t image_index{0};
    std::uint32_t array_index{0};
    XrRect2Di rect{};

    bool Matches(const FrameGenImage& image) const {
        return valid && swapchain == image.swapchain && image_index == image.image_index &&
               array_index == image.array_index && rect.offset.x == image.rect.offset.x &&
               rect.offset.y == image.rect.offset.y && rect.extent.width == image.rect.extent.width &&
               rect.extent.height == image.rect.extent.height;
    }
};

struct ViewResources {
    FrameGenHistoryState history_state;
    CaptureKey capture;
    std::array<Texture, 2> history;
    std::array<Texture, 2> filtered;
    std::array<Texture, kFrameGenPyramidLevels + 1> luma_newest;
    std::array<Texture, kFrameGenPyramidLevels + 1> luma_previous;
    std::array<Texture, kFrameGenPyramidLevels + 1> flow;
    Texture output;
    Texture depth;            // newest frame's linear depth, half resolution
    bool depth_ready{false};  // captured since the last Execute
    bool depth_fresh{false};  // captured for this Execute's frame (for the check)
    std::uint32_t depth_generation{0}; // FrameGenDepthValidator::Generation() the capture decoded with
    FrameGenDepthEncoding depth_encoding; // as the game describes it
};

struct FrameSlot {
    ComPtr<ID3D12CommandAllocator> allocator;
    UINT64 fence_value{0};
    bool timing_pending{false};
    ComPtr<ID3D12Resource> check_readback; // depth check scores, read once the slot's fence passes
    bool check_pending{false};
    std::uint32_t check_generation{0}; // decoding the checked depth used
};

struct CaptureSlot {
    ComPtr<ID3D12CommandAllocator> allocator;
    UINT64 fence_value{0};
};

class D3D12FrameGen final : public FrameGenGpu {
  public:
    explicit D3D12FrameGen(const XrGraphicsBindingD3D12KHR& binding) : device_(binding.device), queue_(binding.queue) {}
    ~D3D12FrameGen() override { Reset(); }

    bool Initialize(std::string& error) {
        auto check = [&](HRESULT hr, const char* operation) {
            if (FAILED(hr)) error = Failure(operation, hr);
            return SUCCEEDED(hr);
        };
        D3D12_DESCRIPTOR_RANGE ranges[2]{};
        ranges[0] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 4, 0, 0, 0};
        ranges[1] = {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 2, 0, 0, 0};
        D3D12_ROOT_PARAMETER parameters[3]{};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[0].Constants = {0, 0, sizeof(FrameGenConstants) / 4};
        parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        for (int i = 0; i < 2; ++i) {
            parameters[1 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            parameters[1 + i].DescriptorTable = {1, &ranges[i]};
            parameters[1 + i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        }
        D3D12_STATIC_SAMPLER_DESC sampler{};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC root{3, parameters, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        ComPtr<ID3DBlob> serialized, diagnostics;
        if (!check(D3D12SerializeRootSignature(&root, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &diagnostics),
                   "D3D12 Frame Generation root signature") ||
            !check(device_->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                                IID_PPV_ARGS(&root_signature_)),
                   "D3D12 Frame Generation root signature object")) {
            return false;
        }
        for (std::size_t pass = 0; pass < kFrameGenPassCount; ++pass) {
            D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
            desc.pRootSignature = root_signature_.Get();
            desc.CS = {framegen_shaders::kDxbc[pass].data, framegen_shaders::kDxbc[pass].size};
            if (!check(device_->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pipelines_[pass])),
                       "D3D12 Frame Generation pipeline")) {
                return false;
            }
        }
        D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
                                        kDescriptorsPerFrame * kFramesInFlight + kCaptureSlots * kDescriptorsPerDispatch,
                                        D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
        if (!check(device_->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&heap_)), "D3D12 Frame Generation descriptor heap")) {
            return false;
        }
        descriptor_size_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        // Without it generated frames are written by compute and copied.
        D3D12_DESCRIPTOR_HEAP_DESC rtv_heap{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kRenderTargetViews,
                                            D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        if (SUCCEEDED(device_->CreateDescriptorHeap(&rtv_heap, IID_PPV_ARGS(&rtv_heap_)))) {
            rtv_size_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        } else {
            rtv_heap_.Reset();
        }
        for (auto& slot : slots_) {
            if (!check(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&slot.allocator)),
                       "D3D12 Frame Generation allocator")) {
                return false;
            }
        }
        for (auto& slot : capture_slots_) {
            if (!check(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&slot.allocator)),
                       "D3D12 Frame Generation capture allocator")) {
                return false;
            }
        }
        if (!check(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, capture_slots_[0].allocator.Get(), nullptr,
                                              IID_PPV_ARGS(&capture_commands_)),
                   "D3D12 Frame Generation capture list") ||
            !check(capture_commands_->Close(), "D3D12 Frame Generation capture list close")) {
            return false;
        }
        if (!check(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, slots_[0].allocator.Get(), nullptr,
                                              IID_PPV_ARGS(&commands_)),
                   "D3D12 Frame Generation command list") ||
            !check(commands_->Close(), "D3D12 Frame Generation command list close") ||
            !check(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)), "D3D12 Frame Generation fence")) {
            return false;
        }
        event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event_) {
            error = "D3D12 Frame Generation fence event failed";
            return false;
        }
        D3D12_QUERY_HEAP_DESC queries{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, static_cast<UINT>(kFramesInFlight * 2), 0};
        D3D12_HEAP_PROPERTIES readback{D3D12_HEAP_TYPE_READBACK};
        D3D12_RESOURCE_DESC buffer{};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = kFramesInFlight * 2 * sizeof(UINT64);
        buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
        buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(queue_->GetTimestampFrequency(&timestamp_frequency_)) ||
            FAILED(device_->CreateQueryHeap(&queries, IID_PPV_ARGS(&query_heap_))) ||
            FAILED(device_->CreateCommittedResource(&readback, D3D12_HEAP_FLAG_NONE, &buffer,
                                                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback_)))) {
            query_heap_.Reset();
            readback_.Reset();
        }
        return true;
    }

    const char* Name() const override { return "D3D12"; }

    std::optional<FrameGenOutputKind> Classify(std::int64_t format) const override {
        switch (static_cast<DXGI_FORMAT>(format)) {
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
            return FrameGenOutputKind::kSrgb8;
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8X8_UNORM:
        case DXGI_FORMAT_R10G10B10A2_UNORM:
            return FrameGenOutputKind::kUnorm8;
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R11G11B10_FLOAT:
            return FrameGenOutputKind::kFloat16;
        default:
            return std::nullopt;
        }
    }

    std::vector<std::int64_t> OutputFormats(FrameGenOutputKind kind) const override {
        switch (kind) {
        case FrameGenOutputKind::kSrgb8:
            return {DXGI_FORMAT_R8G8B8A8_UNORM_SRGB};
        case FrameGenOutputKind::kUnorm8:
            return {DXGI_FORMAT_R8G8B8A8_UNORM};
        default:
            return {DXGI_FORMAT_R16G16B16A16_FLOAT};
        }
    }

    XrSwapchainUsageFlags RequiredSourceUsage() const override { return 0; }
    XrSwapchainUsageFlags OutputUsage() const override {
        return XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    }

    bool Track(XrSwapchain swapchain, PFN_xrEnumerateSwapchainImages enumerate, std::string& error) override {
        if (images_.count(swapchain)) {
            return true;
        }
        std::uint32_t count = 0;
        XrResult result = enumerate(swapchain, 0, &count, nullptr);
        if (XR_FAILED(result) || count == 0) {
            error = "xrEnumerateSwapchainImages count result=" + std::to_string(result);
            return false;
        }
        std::vector<XrSwapchainImageD3D12KHR> native(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
        result = enumerate(swapchain, count, &count, reinterpret_cast<XrSwapchainImageBaseHeader*>(native.data()));
        if (XR_FAILED(result)) {
            error = "xrEnumerateSwapchainImages result=" + std::to_string(result);
            return false;
        }
        auto& resources = images_[swapchain];
        for (const auto& image : native) {
            resources.push_back(image.texture);
        }
        return true;
    }

    void Forget(XrSwapchain swapchain) override { images_.erase(swapchain); }

    bool Capture(std::size_t index, const FrameGenImage& source, const FrameGenViewRequest& request,
                 FrameGenFlowQuality quality, std::string& error) override {
        const auto it = images_.find(source.swapchain);
        if (it == images_.end() || source.image_index >= it->second.size()) {
            error = "Frame Generation capture image is not tracked";
            return false;
        }
        HRESULT hr = device_->GetDeviceRemovedReason();
        if (FAILED(hr)) {
            error = Failure("D3D12 device", hr);
            return false;
        }
        if (views_.size() <= index) {
            views_.resize(index + 1);
        }
        ViewResources& view = views_[index];
        ID3D12Resource* resource = it->second[source.image_index];
        // Capture never reallocates; Execute sets the view up first.
        if (!view.depth.resource || !view.history_state.Matches(request.extent, resource->GetDesc().Format, request.output,
                                                                FrameGenFinalFlowLevel(quality))) {
            error.clear();
            return false;
        }
        CaptureSlot& slot = capture_slots_[capture_count_ % capture_slots_.size()];
        if (!WaitFence(slot.fence_value, 100)) {
            error = "D3D12 Frame Generation capture work did not finish in time";
            return false;
        }
        hr = slot.allocator->Reset();
        if (SUCCEEDED(hr)) hr = capture_commands_->Reset(slot.allocator.Get(), nullptr);
        if (FAILED(hr)) {
            error = Failure("D3D12 Frame Generation capture reset", hr);
            return false;
        }
        std::vector<D3D12_RESOURCE_BARRIER> barriers;
        CopySource(capture_commands_.Get(), barriers, view, source, resource);
        hr = capture_commands_->Close();
        if (FAILED(hr)) {
            // Nothing was submitted, so tracked states no longer match the GPU:
            // force a full rebuild, which waits for the GPU before freeing.
            error = Failure("D3D12 Frame Generation capture close", hr);
            view.history_state = {};
            return false;
        }
        ID3D12CommandList* lists[] = {capture_commands_.Get()};
        queue_->ExecuteCommandLists(1, lists);
        hr = queue_->Signal(fence_.Get(), ++fence_value_);
        if (FAILED(hr)) {
            error = Failure("D3D12 Frame Generation capture signal", hr);
            signal_failed_ = true;
            return false;
        }
        slot.fence_value = fence_value_;
        ++capture_count_;
        view.capture = {true, source.swapchain, source.image_index, source.array_index, source.rect};
        return true;
    }

    bool WaitIdle() override { return WaitFence(fence_value_, 2000); }

    bool SupportsDepth() const override { return true; }
    XrSwapchainUsageFlags RequiredDepthUsage() const override { return XR_SWAPCHAIN_USAGE_SAMPLED_BIT; }

    // The plane-0 view of a depth format, readable in a shader.
    static DXGI_FORMAT DepthViewFormat(DXGI_FORMAT format) {
        switch (format) {
        case DXGI_FORMAT_D32_FLOAT:
        case DXGI_FORMAT_R32_TYPELESS:
        case DXGI_FORMAT_R32_FLOAT:
            return DXGI_FORMAT_R32_FLOAT;
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        case DXGI_FORMAT_R32G8X24_TYPELESS:
            return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
        case DXGI_FORMAT_R24G8_TYPELESS:
            return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        case DXGI_FORMAT_D16_UNORM:
        case DXGI_FORMAT_R16_TYPELESS:
            return DXGI_FORMAT_R16_UNORM;
        default:
            return DXGI_FORMAT_UNKNOWN;
        }
    }

    // Depth swapchain images are in DEPTH_WRITE whenever the application
    // releases them (XR_KHR_D3D12_enable); called before that release is
    // forwarded, like Capture.
    bool CaptureDepth(std::size_t index, const FrameGenImage& depth, const FrameGenDepthEncoding& encoding,
                      const FrameGenViewRequest& request, std::string& error) override {
        error.clear();
        const auto it = images_.find(depth.swapchain);
        if (it == images_.end() || depth.image_index >= it->second.size()) {
            error = "Frame Generation depth image is not tracked";
            return false;
        }
        if (index >= views_.size()) {
            return false;
        }
        ViewResources& view = views_[index];
        const FrameGenExtent extent = view.history_state.Extent();
        if (!view.depth.resource || extent.width != request.extent.width || extent.height != request.extent.height) {
            return false; // Execute sets the view up first
        }
        ID3D12Resource* resource = it->second[depth.image_index];
        const D3D12_RESOURCE_DESC desc = resource->GetDesc();
        const DXGI_FORMAT view_format = DepthViewFormat(desc.Format);
        if (view_format == DXGI_FORMAT_UNKNOWN || desc.SampleDesc.Count != 1 ||
            depth.array_index >= desc.DepthOrArraySize) {
            error = "Frame Generation cannot read depth format " + std::to_string(static_cast<int>(desc.Format));
            return false;
        }
        HRESULT hr = device_->GetDeviceRemovedReason();
        if (FAILED(hr)) {
            error = Failure("D3D12 device", hr);
            return false;
        }
        const std::size_t slot_index = capture_count_ % capture_slots_.size();
        CaptureSlot& slot = capture_slots_[slot_index];
        if (!WaitFence(slot.fence_value, 100)) {
            error = "D3D12 Frame Generation capture work did not finish in time";
            return false;
        }
        hr = slot.allocator->Reset();
        if (SUCCEEDED(hr)) hr = capture_commands_->Reset(slot.allocator.Get(), nullptr);
        if (FAILED(hr)) {
            error = Failure("D3D12 Frame Generation depth capture reset", hr);
            return false;
        }
        ID3D12DescriptorHeap* heaps[] = {heap_.Get()};
        capture_commands_->SetDescriptorHeaps(1, heaps);
        capture_commands_->SetComputeRootSignature(root_signature_.Get());
        const UINT base = kDescriptorsPerFrame * kFramesInFlight + static_cast<UINT>(slot_index) * kDescriptorsPerDispatch;
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = view_format;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2DArray.MipLevels = 1;
        srv.Texture2DArray.ArraySize = desc.DepthOrArraySize;
        device_->CreateShaderResourceView(resource, &srv, Cpu(base));
        for (UINT i = 1; i < 4; ++i) {
            D3D12_SHADER_RESOURCE_VIEW_DESC none{};
            none.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            none.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            none.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            none.Texture2D.MipLevels = 1;
            device_->CreateShaderResourceView(nullptr, &none, Cpu(base + i));
        }
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = DXGI_FORMAT_R32_FLOAT;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device_->CreateUnorderedAccessView(view.depth.resource.Get(), nullptr, &uav, Cpu(base + 4));
        D3D12_UNORDERED_ACCESS_VIEW_DESC no_uav = uav;
        no_uav.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        device_->CreateUnorderedAccessView(nullptr, nullptr, &no_uav, Cpu(base + 5));

        std::vector<D3D12_RESOURCE_BARRIER> barriers;
        auto flush = [&] {
            if (!barriers.empty()) {
                capture_commands_->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
                barriers.clear();
            }
        };
        Transition(barriers, resource, D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Require(barriers, view.depth, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        flush();
        view.depth_encoding = encoding;
        view.depth_generation = validator_.Generation();
        FrameGenDepthEncoding depth_encoding = encoding;
        if (validator_.Swapped()) {
            std::swap(depth_encoding.near_z, depth_encoding.far_z);
        }
        const FrameGenDispatch dispatch = BuildFrameGenDepthDispatch(
            request.extent, depth.rect.offset.x, depth.rect.offset.y, static_cast<std::uint32_t>(depth.rect.extent.width),
            static_cast<std::uint32_t>(depth.rect.extent.height), depth.array_index, depth_encoding);
        capture_commands_->SetPipelineState(pipelines_[static_cast<std::size_t>(FrameGenPass::kDepth)].Get());
        capture_commands_->SetComputeRoot32BitConstants(0, sizeof(FrameGenConstants) / 4, &dispatch.constants, 0);
        capture_commands_->SetComputeRootDescriptorTable(1, Gpu(base));
        capture_commands_->SetComputeRootDescriptorTable(2, Gpu(base + 4));
        capture_commands_->Dispatch((dispatch.width + kFrameGenThreadGroupSize - 1) / kFrameGenThreadGroupSize,
                                    (dispatch.height + kFrameGenThreadGroupSize - 1) / kFrameGenThreadGroupSize, 1);
        Transition(barriers, resource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        flush();
        hr = capture_commands_->Close();
        if (FAILED(hr)) {
            error = Failure("D3D12 Frame Generation depth capture close", hr);
            view.history_state = {};
            return false;
        }
        ID3D12CommandList* lists[] = {capture_commands_.Get()};
        queue_->ExecuteCommandLists(1, lists);
        hr = queue_->Signal(fence_.Get(), ++fence_value_);
        if (FAILED(hr)) {
            error = Failure("D3D12 Frame Generation depth capture signal", hr);
            signal_failed_ = true;
            return false;
        }
        slot.fence_value = fence_value_;
        ++capture_count_;
        view.depth_ready = true;
        view.depth_fresh = true;
        return true;
    }

    FrameGenDepthStatus DepthStatus() const override {
        return {validator_.Reading(), validator_.Decisions(), validator_.Evidence()};
    }

    // Negates the texels of `view`'s depth that `other` disproves (see
    // FG_PASS_DEPTH_TRUST); both views' newest images must be this frame's.
    bool RecordDepthTrust(const FrameGenViewSubmission& submission, const FrameGenViewSubmission& other_submission,
                          ViewResources& view, ViewResources& other, std::string& error) {
        if (descriptor_cursor_ + kDescriptorsPerDispatch > descriptor_end_) {
            error = "D3D12 Frame Generation descriptor budget exhausted";
            return false;
        }
        const UINT base = descriptor_cursor_;
        descriptor_cursor_ += kDescriptorsPerDispatch;
        Texture* inputs[4] = {&view.history[view.history_state.Newest()], &other.history[other.history_state.Newest()],
                              nullptr, nullptr};
        std::vector<D3D12_RESOURCE_BARRIER> barriers;
        for (UINT i = 0; i < 4; ++i) {
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.Format = inputs[i] ? inputs[i]->view_format : DXGI_FORMAT_R8G8B8A8_UNORM;
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Texture2D.MipLevels = 1;
            device_->CreateShaderResourceView(inputs[i] ? inputs[i]->resource.Get() : nullptr, &srv, Cpu(base + i));
            if (inputs[i]) Require(barriers, *inputs[i], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = DXGI_FORMAT_R32_FLOAT;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device_->CreateUnorderedAccessView(view.depth.resource.Get(), nullptr, &uav, Cpu(base + 4));
        D3D12_UNORDERED_ACCESS_VIEW_DESC no_uav = uav;
        no_uav.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        device_->CreateUnorderedAccessView(nullptr, nullptr, &no_uav, Cpu(base + 5));
        Require(barriers, view.depth, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Flush(barriers);
        const FrameGenDispatch dispatch = BuildFrameGenDepthTrustDispatch(submission.request.extent, submission.request.newest,
                                                                          other_submission.request.newest);
        commands_->SetPipelineState(pipelines_[static_cast<std::size_t>(FrameGenPass::kDepthTrust)].Get());
        commands_->SetComputeRoot32BitConstants(0, sizeof(FrameGenConstants) / 4, &dispatch.constants, 0);
        commands_->SetComputeRootDescriptorTable(1, Gpu(base));
        commands_->SetComputeRootDescriptorTable(2, Gpu(base + 4));
        commands_->Dispatch((dispatch.width + kFrameGenThreadGroupSize - 1) / kFrameGenThreadGroupSize,
                            (dispatch.height + kFrameGenThreadGroupSize - 1) / kFrameGenThreadGroupSize, 1);
        return true;
    }

    // Scores the depth of view 0 against view 1 (see FrameGenDepthValidator)
    // and copies the scores for reading once this submission has finished.
    bool RecordDepthCheck(const FrameGenViewSubmission& first, const FrameGenViewSubmission& second,
                          ViewResources& view0, ViewResources& view1, FrameSlot& slot, std::string& error) {
        const FrameGenExtent cells{kFrameGenDepthCheckCells * kFrameGenDepthCheckTexels, kFrameGenDepthCheckCells};
        if (!check_scores_.resource &&
            !Create(check_scores_, cells, DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT, true, error)) {
            return false;
        }
        const D3D12_RESOURCE_DESC desc = check_scores_.resource->GetDesc();
        device_->GetCopyableFootprints(&desc, 0, 1, 0, &check_footprint_, nullptr, nullptr, &check_bytes_);
        if (!slot.check_readback) {
            D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_READBACK};
            D3D12_RESOURCE_DESC buffer{};
            buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            buffer.Width = check_bytes_;
            buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
            buffer.SampleDesc.Count = 1;
            buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            const HRESULT hr = device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                                                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                                IID_PPV_ARGS(&slot.check_readback));
            if (FAILED(hr)) {
                error = Failure("D3D12 Frame Generation depth check readback", hr);
                return false;
            }
        }
        if (descriptor_cursor_ + kDescriptorsPerDispatch > descriptor_end_) {
            error = "D3D12 Frame Generation descriptor budget exhausted";
            return false;
        }
        const UINT base = descriptor_cursor_;
        descriptor_cursor_ += kDescriptorsPerDispatch;
        Texture* inputs[4] = {&view0.history[view0.history_state.Newest()], &view1.history[view1.history_state.Newest()],
                              &view0.depth, nullptr};
        std::vector<D3D12_RESOURCE_BARRIER> barriers;
        for (UINT i = 0; i < 4; ++i) {
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.Format = inputs[i] ? inputs[i]->view_format : DXGI_FORMAT_R8G8B8A8_UNORM;
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Texture2D.MipLevels = 1;
            device_->CreateShaderResourceView(inputs[i] ? inputs[i]->resource.Get() : nullptr, &srv, Cpu(base + i));
            if (inputs[i]) Require(barriers, *inputs[i], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device_->CreateUnorderedAccessView(check_scores_.resource.Get(), nullptr, &uav, Cpu(base + 4));
        D3D12_UNORDERED_ACCESS_VIEW_DESC no_uav = uav;
        no_uav.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        device_->CreateUnorderedAccessView(nullptr, nullptr, &no_uav, Cpu(base + 5));
        Require(barriers, check_scores_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Flush(barriers);
        const FrameGenDispatch dispatch =
            BuildFrameGenDepthCheckDispatch(first.request.newest, second.request.newest, view0.depth_encoding);
        commands_->SetPipelineState(pipelines_[static_cast<std::size_t>(FrameGenPass::kDepthCheck)].Get());
        commands_->SetComputeRoot32BitConstants(0, sizeof(FrameGenConstants) / 4, &dispatch.constants, 0);
        commands_->SetComputeRootDescriptorTable(1, Gpu(base));
        commands_->SetComputeRootDescriptorTable(2, Gpu(base + 4));
        commands_->Dispatch((dispatch.width + kFrameGenThreadGroupSize - 1) / kFrameGenThreadGroupSize,
                            (dispatch.height + kFrameGenThreadGroupSize - 1) / kFrameGenThreadGroupSize, 1);
        Require(barriers, check_scores_, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Flush(barriers);
        D3D12_TEXTURE_COPY_LOCATION destination{slot.check_readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        destination.PlacedFootprint = check_footprint_;
        D3D12_TEXTURE_COPY_LOCATION source{check_scores_.resource.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        source.SubresourceIndex = 0;
        commands_->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        slot.check_pending = true;
        slot.check_generation = view0.depth_generation;
        return true;
    }

    // Feeds a finished check's scores to the validator.
    void ReadDepthCheck(FrameSlot& slot) {
        if (!slot.check_pending || !slot.check_readback) {
            return;
        }
        slot.check_pending = false;
        void* mapped = nullptr;
        const D3D12_RANGE range{0, static_cast<SIZE_T>(check_bytes_)};
        if (FAILED(slot.check_readback->Map(0, &range, &mapped)) || !mapped) {
            return;
        }
        FrameGenDepthEvidence evidence;
        FrameGenDepthScore* scores[] = {&evidence.decoded, &evidence.swapped};
        for (UINT y = 0; y < kFrameGenDepthCheckCells; ++y) {
            const auto* row = reinterpret_cast<const float*>(static_cast<const std::byte*>(mapped) +
                                                             check_footprint_.Offset + y * check_footprint_.Footprint.RowPitch);
            for (UINT x = 0; x < kFrameGenDepthCheckCells * kFrameGenDepthCheckTexels; ++x) {
                const float* texel = row + x * 4;
                const UINT reading = x % kFrameGenDepthCheckTexels;
                if (reading < 2) {
                    scores[reading]->error += texel[0];
                    scores[reading]->none += texel[1];
                    scores[reading]->samples += texel[2];
                    scores[reading]->fallback += texel[3];
                } else {
                    evidence.binocular += texel[0];
                }
            }
        }
        const D3D12_RANGE written{0, 0};
        slot.check_readback->Unmap(0, &written);
        // A check of depth decoded before the validator last changed the
        // decoding scored other readings.
        if (slot.check_generation == validator_.Generation()) {
            validator_.Add(evidence);
        }
    }

    bool SupportsThreadedPresentation() const override { return true; }

    void SetProfiling(bool on) override {
        profiling_ = false;
        if (!on) {
            return;
        }
        D3D12_QUERY_HEAP_DESC queries{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, kProfileStamps, 0};
        D3D12_HEAP_PROPERTIES readback{D3D12_HEAP_TYPE_READBACK};
        D3D12_RESOURCE_DESC buffer{};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = kProfileStamps * sizeof(UINT64);
        buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
        buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        profiling_ = timestamp_frequency_ != 0 &&
                     (profile_heap_ || SUCCEEDED(device_->CreateQueryHeap(&queries, IID_PPV_ARGS(&profile_heap_)))) &&
                     (profile_readback_ ||
                      SUCCEEDED(device_->CreateCommittedResource(&readback, D3D12_HEAP_FLAG_NONE, &buffer,
                                                                 D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                                 IID_PPV_ARGS(&profile_readback_))));
    }

    std::vector<double> PassMilliseconds() const override { return profile_ms_; }

    // The graphics pipeline drawing `count` generated frames into render
    // targets of these formats; null if it cannot be created.
    ID3D12PipelineState* DrawPipeline(const std::array<DXGI_FORMAT, kFrameGenMaxGenerated>& formats, std::size_t count) {
        std::uint64_t key = count;
        for (std::size_t i = 0; i < count; ++i) {
            key = key * 1000 + static_cast<std::uint64_t>(formats[i]);
        }
        const auto found = draw_pipelines_.find(key);
        if (found != draw_pipelines_.end()) {
            return found->second.Get();
        }
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = root_signature_.Get();
        desc.VS = {framegen_shaders::kDxbcDraw[0].data, framegen_shaders::kDxbcDraw[0].size};
        desc.PS = {framegen_shaders::kDxbcDraw[count].data, framegen_shaders::kDxbcDraw[count].size};
        for (std::size_t i = 0; i < count; ++i) {
            desc.BlendState.RenderTarget[i].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            desc.RTVFormats[i] = formats[i];
        }
        desc.SampleMask = UINT_MAX;
        desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        desc.RasterizerState.DepthClipEnable = TRUE;
        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.NumRenderTargets = static_cast<UINT>(count);
        desc.SampleDesc.Count = 1;
        ComPtr<ID3D12PipelineState> pipeline;
        if (FAILED(device_->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline)))) {
            pipeline.Reset();
        }
        draw_pipelines_[key] = pipeline;
        return pipeline.Get();
    }

    // Whether every generated frame of the view can be drawn straight into
    // its output image: renderable, single-sampled, a typed format, and the
    // whole image at the view's size.
    bool CanDraw(const FrameGenViewSubmission& submission, const std::vector<ID3D12Resource*>& outputs,
                 std::size_t count) {
        if (!direct_output_ || !rtv_heap_ || count == 0 || count > kFrameGenMaxGenerated || outputs.size() < count) {
            return false;
        }
        std::array<DXGI_FORMAT, kFrameGenMaxGenerated> formats{};
        for (std::size_t i = 0; i < count; ++i) {
            formats[i] = static_cast<DXGI_FORMAT>(submission.outputs[i].format);
        }
        for (std::size_t i = 0; i < count; ++i) {
            const FrameGenImage& image = submission.outputs[i];
            const D3D12_RESOURCE_DESC desc = outputs[i]->GetDesc();
            if (!(desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) || desc.SampleDesc.Count != 1 ||
                image.array_index >= desc.DepthOrArraySize || image.rect.offset.x != 0 || image.rect.offset.y != 0 ||
                static_cast<std::uint32_t>(image.rect.extent.width) != submission.request.extent.width ||
                static_cast<std::uint32_t>(image.rect.extent.height) != submission.request.extent.height ||
                image.format == DXGI_FORMAT_R8G8B8A8_TYPELESS || image.format == DXGI_FORMAT_B8G8R8A8_TYPELESS ||
                image.format == DXGI_FORMAT_R16G16B16A16_TYPELESS || image.format == DXGI_FORMAT_UNKNOWN) {
                return false;
            }
        }
        return DrawPipeline(formats, count) != nullptr;
    }

    void SetDirectOutput(bool on) override { direct_output_ = on; }
    bool DrawsOutputs() const override { return direct_output_ && rtv_heap_; }

    // Draws the view's generated frames (the plan's synthesis dispatches, one
    // per output) straight into the output images in one draw.
    bool RecordDraw(ViewResources& view, const FrameGenViewSubmission& submission,
                    const std::vector<ID3D12Resource*>& outputs, const std::vector<const FrameGenDispatch*>& frames,
                    std::string& error) {
        std::array<DXGI_FORMAT, kFrameGenMaxGenerated> formats{};
        for (std::size_t i = 0; i < frames.size(); ++i) {
            formats[i] = static_cast<DXGI_FORMAT>(submission.outputs[i].format);
        }
        ID3D12PipelineState* pipeline = DrawPipeline(formats, frames.size()); // created by CanDraw
        if (descriptor_cursor_ + kDescriptorsPerDispatch > descriptor_end_) {
            error = "D3D12 Frame Generation descriptor budget exhausted";
            return false;
        }
        const FrameGenDispatch& first = *frames[0];
        const UINT base = descriptor_cursor_;
        descriptor_cursor_ += kDescriptorsPerDispatch;
        std::vector<D3D12_RESOURCE_BARRIER> barriers;
        for (std::size_t i = 0; i < 4; ++i) {
            Texture* texture = Resolve(view, first.inputs[i]);
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.Format = texture ? texture->view_format : DXGI_FORMAT_R8G8B8A8_UNORM;
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Texture2D.MipLevels = 1;
            device_->CreateShaderResourceView(texture ? texture->resource.Get() : nullptr, &srv,
                                              Cpu(base + static_cast<UINT>(i)));
            if (texture) Require(barriers, *texture, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
        }
        D3D12_UNORDERED_ACCESS_VIEW_DESC no_uav{};
        no_uav.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        no_uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device_->CreateUnorderedAccessView(nullptr, nullptr, &no_uav, Cpu(base + 4));
        device_->CreateUnorderedAccessView(nullptr, nullptr, &no_uav, Cpu(base + 5));
        Flush(barriers);

        // Output images are in RENDER_TARGET while acquired.
        std::array<D3D12_CPU_DESCRIPTOR_HANDLE, kFrameGenMaxGenerated> targets{};
        for (std::size_t i = 0; i < frames.size(); ++i) {
            const FrameGenImage& image = submission.outputs[i];
            D3D12_RENDER_TARGET_VIEW_DESC rtv{};
            rtv.Format = formats[i];
            if (outputs[i]->GetDesc().DepthOrArraySize > 1) {
                rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
                rtv.Texture2DArray.FirstArraySlice = image.array_index;
                rtv.Texture2DArray.ArraySize = 1;
            } else {
                rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
            }
            targets[i] = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
            targets[i].ptr += static_cast<SIZE_T>(rtv_cursor_++ % kRenderTargetViews) * rtv_size_;
            device_->CreateRenderTargetView(outputs[i], &rtv, targets[i]);
        }
        FrameGenConstants constants = first.constants;
        constants.options[1] &= ~kFrameGenFlagSrgbEncode; // the sRGB render target encodes
        for (std::size_t i = 1; i < frames.size(); ++i) {
            const float fraction = frames[i]->constants.tuning[0];
            std::memcpy(&constants.size[1 + i], &fraction, sizeof(fraction));
        }
        const D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(submission.request.extent.width),
                                      static_cast<float>(submission.request.extent.height), 0.0f, 1.0f};
        const D3D12_RECT scissor{0, 0, static_cast<LONG>(submission.request.extent.width),
                                 static_cast<LONG>(submission.request.extent.height)};
        commands_->SetGraphicsRootSignature(root_signature_.Get());
        commands_->SetPipelineState(pipeline);
        commands_->SetGraphicsRoot32BitConstants(0, sizeof(FrameGenConstants) / 4, &constants, 0);
        commands_->SetGraphicsRootDescriptorTable(1, Gpu(base));
        commands_->SetGraphicsRootDescriptorTable(2, Gpu(base + 4));
        commands_->RSSetViewports(1, &viewport);
        commands_->RSSetScissorRects(1, &scissor);
        commands_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        commands_->OMSetRenderTargets(static_cast<UINT>(frames.size()), targets.data(), FALSE, nullptr);
        commands_->DrawInstanced(3, 1, 0, 0);
        Stamp(static_cast<int>(FrameGenPass::kSynthesizeUnorm8));
        return true;
    }

    // Profiling: what ran since the previous stamp.
    static constexpr int kProfileSourceCopy = static_cast<int>(kFrameGenPassCount);
    static constexpr int kProfileOutputCopy = kProfileSourceCopy + 1;
    static constexpr UINT kProfileStamps = 1024;

    void Stamp(int category) {
        if (!profiling_ || profile_labels_.size() + 1 >= kProfileStamps) {
            return;
        }
        commands_->EndQuery(profile_heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, static_cast<UINT>(profile_labels_.size() + 1));
        profile_labels_.push_back(category);
    }

    // After the profiled submission: waits for it and sums the time per category.
    void ReadProfile() {
        profile_ms_.assign(kProfileOutputCopy + 1, 0.0);
        if (!WaitIdle()) {
            return;
        }
        void* mapped = nullptr;
        const D3D12_RANGE range{0, (profile_labels_.size() + 1) * sizeof(UINT64)};
        if (FAILED(profile_readback_->Map(0, &range, &mapped))) {
            return;
        }
        const auto* stamps = static_cast<const UINT64*>(mapped);
        for (std::size_t i = 0; i < profile_labels_.size(); ++i) {
            if (stamps[i + 1] >= stamps[i]) {
                profile_ms_[profile_labels_[i]] +=
                    static_cast<double>(stamps[i + 1] - stamps[i]) * 1000.0 / static_cast<double>(timestamp_frequency_);
            }
        }
        const D3D12_RANGE written{0, 0};
        profile_readback_->Unmap(0, &written);
    }

    bool Execute(const std::vector<FrameGenViewSubmission>& views, const FrameGenTuning& tuning,
                 FrameGenExecuteResult& result, std::string& error) override {
        result.synthesized.assign(views.size(), false);
        result.depth.assign(views.size(), false);
        HRESULT hr = device_->GetDeviceRemovedReason();
        if (FAILED(hr)) {
            error = Failure("D3D12 device", hr);
            return false;
        }
        const std::size_t slot_index = static_cast<std::size_t>(submission_ % kFramesInFlight);
        FrameSlot& slot = slots_[slot_index];
        if (!WaitForSlot(slot, error)) {
            return false;
        }
        ReadTiming(slot, slot_index);
        ReadDepthCheck(slot);
        hr = slot.allocator->Reset();
        if (SUCCEEDED(hr)) hr = commands_->Reset(slot.allocator.Get(), nullptr);
        if (FAILED(hr)) {
            error = Failure("D3D12 Frame Generation command reset", hr);
            return false;
        }
        descriptor_cursor_ = static_cast<UINT>(slot_index) * kDescriptorsPerFrame;
        descriptor_end_ = descriptor_cursor_ + kDescriptorsPerFrame;
        ID3D12DescriptorHeap* heaps[] = {heap_.Get()};
        commands_->SetDescriptorHeaps(1, heaps);
        commands_->SetComputeRootSignature(root_signature_.Get());
        const bool timed = query_heap_ != nullptr;
        if (timed) commands_->EndQuery(query_heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, static_cast<UINT>(slot_index * 2));
        profile_labels_.clear();
        if (profiling_) commands_->EndQuery(profile_heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);

        if (views_.size() < views.size()) {
            views_.resize(views.size());
        }
        bool ok = true;
        // Depth the other eye disproves is dropped per texel before any view
        // uses it. Needs both eyes' images of this frame, captured at release.
        if (views.size() >= 2 && validator_.Usable()) {
            bool both = true;
            for (std::size_t i = 0; i < 2; ++i) {
                both = both && Prepared(views[i], views_[i], tuning) && views_[i].capture.Matches(views[i].source);
            }
            for (std::size_t i = 0; i < 2 && both && ok; ++i) {
                ViewResources& view = views_[i];
                if (views[i].request.depth && view.depth_ready && view.depth_generation == validator_.Generation()) {
                    ok = RecordDepthTrust(views[i], views[1 - i], view, views_[1 - i], error);
                    Stamp(static_cast<int>(FrameGenPass::kDepthTrust));
                }
            }
        }
        for (std::size_t i = 0; i < views.size() && ok; ++i) {
            bool synthesized = false;
            bool depth = false;
            ok = RecordView(views[i], tuning, views_[i], synthesized, depth, error);
            result.synthesized[i] = synthesized;
            result.depth[i] = depth;
        }
        // Check the game's depth against the other eye while undecided, and
        // now and then afterwards.
        if (ok && views.size() >= 2 && views_[0].depth_fresh && views[0].request.depth &&
            views_[0].depth_generation == validator_.Generation() && validator_.WantsCheck(submission_)) {
            ok = RecordDepthCheck(views[0], views[1], views_[0], views_[1], slot, error);
            Stamp(static_cast<int>(FrameGenPass::kDepthCheck));
        }
        for (std::size_t i = 0; i < views.size(); ++i) {
            views_[i].depth_fresh = false;
        }
        if (timed) {
            commands_->EndQuery(query_heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, static_cast<UINT>(slot_index * 2 + 1));
            commands_->ResolveQueryData(query_heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, static_cast<UINT>(slot_index * 2), 2,
                                        readback_.Get(), slot_index * 2 * sizeof(UINT64));
        }
        if (profiling_) {
            commands_->ResolveQueryData(profile_heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0,
                                        static_cast<UINT>(profile_labels_.size() + 1), profile_readback_.Get(), 0);
        }
        hr = commands_->Close();
        if (FAILED(hr)) {
            error = Failure("D3D12 Frame Generation command close", hr);
            ok = false;
        }
        if (!ok) {
            // Nothing was submitted: the recorded state transitions never ran.
            // Rebuild the views, freeing textures only once earlier
            // submissions have finished with them.
            const bool idle = WaitIdle();
            for (std::size_t i = 0; i < views.size() && i < views_.size(); ++i) {
                if (idle) {
                    views_[i] = {};
                } else {
                    views_[i].history_state = {};
                }
            }
            return false;
        }
        ID3D12CommandList* lists[] = {commands_.Get()};
        queue_->ExecuteCommandLists(1, lists);
        hr = queue_->Signal(fence_.Get(), ++fence_value_);
        if (FAILED(hr)) {
            error = Failure("D3D12 Frame Generation queue signal", hr);
            signal_failed_ = true;
            return false;
        }
        slot.fence_value = fence_value_;
        slot.timing_pending = timed;
        ++submission_;
        if (profiling_) {
            ReadProfile();
        }
        for (std::size_t i = 0; i < views.size(); ++i) {
            views_[i].history_state.Commit(result.synthesized[i]);
            views_[i].capture.valid = false;
        }
        return true;
    }

    void ResetHistory() override {
        for (auto& view : views_) {
            view.history_state.Invalidate();
        }
    }

    void Reset() override {
        if (!WaitIdle()) {
            // Teardown with a stalled GPU: leak rather than free in-use memory.
            for (auto& view : views_) {
                for (auto* group : {&view.history, &view.filtered}) {
                    for (auto& texture : *group) texture.resource.Detach();
                }
                for (auto* group : {&view.luma_newest, &view.luma_previous, &view.flow}) {
                    for (auto& texture : *group) texture.resource.Detach();
                }
                view.output.resource.Detach();
            }
        }
        views_.clear();
        images_.clear();
        if (event_) {
            CloseHandle(event_);
            event_ = nullptr;
        }
    }

    double LastGpuMilliseconds() const override { return last_gpu_ms_; }

  private:
    // Every wait shares one auto-reset event, so an earlier wait that timed
    // out can wake a later one early: wake, then re-check the fence itself.
    bool WaitFence(UINT64 value, DWORD timeout_ms) {
        if (!fence_ || !value || fence_->GetCompletedValue() >= value) {
            return true;
        }
        if (signal_failed_ || !event_ || FAILED(fence_->SetEventOnCompletion(value, event_))) {
            return false;
        }
        const ULONGLONG deadline = GetTickCount64() + timeout_ms;
        while (fence_->GetCompletedValue() < value) {
            const ULONGLONG now = GetTickCount64();
            if (now >= deadline) {
                return false;
            }
            WaitForSingleObject(event_, static_cast<DWORD>(std::min<ULONGLONG>(deadline - now, 10)));
        }
        return true;
    }

    bool WaitForSlot(FrameSlot& slot, std::string& error) {
        // Three submissions ago; only a stalled GPU waits here.
        if (!WaitFence(slot.fence_value, 100)) {
            error = "D3D12 Frame Generation GPU work did not finish in time";
            return false;
        }
        return true;
    }

    void ReadTiming(FrameSlot& slot, std::size_t slot_index) {
        if (!slot.timing_pending || !readback_ || !timestamp_frequency_) {
            return;
        }
        slot.timing_pending = false;
        const D3D12_RANGE range{slot_index * 2 * sizeof(UINT64), (slot_index * 2 + 2) * sizeof(UINT64)};
        void* mapped = nullptr;
        if (SUCCEEDED(readback_->Map(0, &range, &mapped))) {
            const auto* stamps = reinterpret_cast<const UINT64*>(static_cast<const std::byte*>(mapped) + range.Begin);
            if (stamps[1] >= stamps[0]) {
                last_gpu_ms_ = static_cast<double>(stamps[1] - stamps[0]) * 1000.0 / static_cast<double>(timestamp_frequency_);
            }
            const D3D12_RANGE written{0, 0};
            readback_->Unmap(0, &written);
        }
    }

    bool Create(Texture& texture, FrameGenExtent extent, DXGI_FORMAT format, DXGI_FORMAT view_format, bool uav,
                std::string& error) {
        texture = {};
        D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = extent.width;
        desc.Height = extent.height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
        const D3D12_RESOURCE_STATES initial = uav ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_COPY_DEST;
        const HRESULT hr = device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, initial, nullptr,
                                                            IID_PPV_ARGS(&texture.resource));
        if (FAILED(hr)) {
            error = Failure("D3D12 Frame Generation texture", hr);
            texture = {};
            return false;
        }
        texture.view_format = view_format;
        texture.state = initial;
        texture.uav = uav;
        return true;
    }

    // Whether EnsureView would keep every texture of the view as it is.
    bool Prepared(const FrameGenViewSubmission& submission, const ViewResources& view, const FrameGenTuning& tuning) const {
        const auto it = images_.find(submission.source.swapchain);
        if (it == images_.end() || submission.source.image_index >= it->second.size() || !view.depth.resource) {
            return false;
        }
        return view.history_state.Matches(submission.request.extent, it->second[submission.source.image_index]->GetDesc().Format,
                                          submission.request.output, FrameGenFinalFlowLevel(tuning.quality));
    }

    bool EnsureView(ViewResources& view, const FrameGenViewSubmission& submission, ID3D12Resource* source,
                    const FrameGenTuning& tuning, std::string& error) {
        const D3D12_RESOURCE_DESC source_desc = source->GetDesc();
        const FrameGenExtent extent = submission.request.extent;
        const std::uint8_t final_level = FrameGenFinalFlowLevel(tuning.quality);
        const unsigned change = view.history_state.Configure(extent, source_desc.Format, submission.request.output,
                                                              final_level);
        const auto levels = FrameGenPyramid(extent);
        if (change != FrameGenHistoryState::kNone && !WaitIdle()) {
            // Recent submissions still use the textures being replaced.
            view.history_state = {};
            error = "D3D12 Frame Generation GPU work did not finish before reallocation";
            return false;
        }
        // The depth texture is created last: it marks a view set up.
        if (change & FrameGenHistoryState::kAll || !view.depth.resource) {
            view.capture.valid = false;
            const DXGI_FORMAT view_format = ViewFormat(source_desc.Format, submission.source.format);
            for (auto& history : view.history) {
                if (!Create(history, extent, source_desc.Format, view_format, false, error)) return false;
            }
            for (std::uint8_t level = 1; level <= kFrameGenPyramidLevels; ++level) {
                if (!Create(view.luma_newest[level], levels[level], DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT, true, error) ||
                    !Create(view.luma_previous[level], levels[level], DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT, true, error)) {
                    return false;
                }
                if (level >= 2 && !Create(view.flow[level], levels[level], DXGI_FORMAT_R16G16B16A16_FLOAT,
                                          DXGI_FORMAT_R16G16B16A16_FLOAT, true, error)) {
                    return false;
                }
            }
            view.output = {}; // created when first needed (see EnsureOutput)
            if (!Create(view.depth, levels[1], DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT, true, error)) return false;
            view.depth_ready = false;
        }
        if (change != FrameGenHistoryState::kNone || !view.filtered[0].resource) {
            for (auto& filtered : view.filtered) {
                if (!Create(filtered, levels[final_level], DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT,
                            true, error)) {
                    return false;
                }
            }
        }
        return true;
    }

    // The image generated frames are written to by compute before being
    // copied to the outputs. Not needed while they are drawn straight into
    // the outputs, which saves a headset-resolution image per view.
    bool EnsureOutput(ViewResources& view, const FrameGenViewSubmission& submission, std::string& error) {
        if (view.output.resource) {
            return true;
        }
        const DXGI_FORMAT intermediate = submission.request.output == FrameGenOutputKind::kFloat16
            ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
        return Create(view.output, submission.request.extent, intermediate, intermediate, true, error);
    }

    Texture* Resolve(ViewResources& view, FrameGenTexture texture) {
        const std::uint8_t newest = view.history_state.Newest();
        const std::uint8_t previous = view.history_state.Previous();
        switch (texture.kind) {
        case FrameGenTextureKind::kHistoryNewest: return &view.history[newest];
        case FrameGenTextureKind::kHistoryPrevious: return &view.history[previous];
        case FrameGenTextureKind::kLumaNewest: return &view.luma_newest[texture.level];
        case FrameGenTextureKind::kLumaPrevious: return &view.luma_previous[texture.level];
        case FrameGenTextureKind::kFlow: return &view.flow[texture.level];
        case FrameGenTextureKind::kFilteredNewest: return &view.filtered[newest];
        case FrameGenTextureKind::kFilteredPrevious: return &view.filtered[previous];
        case FrameGenTextureKind::kOutput: return &view.output;
        case FrameGenTextureKind::kDepth: return &view.depth;
        default: return nullptr;
        }
    }

    static void Transition(std::vector<D3D12_RESOURCE_BARRIER>& barriers, ID3D12Resource* resource,
                           D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
        barriers.push_back(barrier);
    }

    static void Require(std::vector<D3D12_RESOURCE_BARRIER>& barriers, Texture& texture, D3D12_RESOURCE_STATES state) {
        if (texture.state == state) {
            if (state == D3D12_RESOURCE_STATE_UNORDERED_ACCESS) {
                D3D12_RESOURCE_BARRIER barrier{};
                barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                barrier.UAV.pResource = texture.resource.Get();
                barriers.push_back(barrier);
            }
            return;
        }
        Transition(barriers, texture.resource.Get(), texture.state, state);
        texture.state = state;
    }

    void Flush(std::vector<D3D12_RESOURCE_BARRIER>& barriers) {
        if (!barriers.empty()) {
            commands_->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
            barriers.clear();
        }
    }

    bool RecordView(const FrameGenViewSubmission& submission, const FrameGenTuning& tuning, ViewResources& view,
                    bool& synthesized, bool& depth_used, std::string& error) {
        const auto source_it = images_.find(submission.source.swapchain);
        if (source_it == images_.end() || submission.source.image_index >= source_it->second.size() ||
            submission.outputs.size() < tuning.generated) {
            error = "Frame Generation image is not tracked";
            return false;
        }
        std::vector<ID3D12Resource*> outputs;
        for (const FrameGenImage& image : submission.outputs) {
            const auto output_it = images_.find(image.swapchain);
            if (output_it == images_.end() || image.image_index >= output_it->second.size()) {
                error = "Frame Generation output image is not tracked";
                return false;
            }
            outputs.push_back(output_it->second[image.image_index]);
        }
        ID3D12Resource* source = source_it->second[submission.source.image_index];
        if (!EnsureView(view, submission, source, tuning, error)) {
            return false;
        }

        std::vector<D3D12_RESOURCE_BARRIER> barriers;
        if (!view.capture.Matches(submission.source)) {
            CopySource(commands_.Get(), barriers, view, submission.source, source);
            Stamp(kProfileSourceCopy);
        }

        // Depth only when it was captured for this frame; it is used once.
        FrameGenViewRequest effective = view.history_state.Effective(submission.request);
        // Only once the depth is known to line the eyes up (see RecordDepthCheck).
        effective.depth = effective.depth && view.depth_ready && view.depth.resource && validator_.Usable() &&
                          view.depth_generation == validator_.Generation();
        view.depth_ready = false;
        depth_used = effective.depth && effective.synthesize;
        const FrameGenViewPlan plan = BuildFrameGenViewPlan(effective, tuning);
        // The generated frames (a synthesis dispatch per output, or one still
        // write for all of them) are drawn into the outputs together when
        // the outputs allow it; they come last in the plan.
        auto generates = [](const FrameGenDispatch& dispatch) {
            return dispatch.pass == FrameGenPass::kSynthesizeUnorm8 || dispatch.pass == FrameGenPass::kSynthesizeFloat16;
        };
        std::vector<const FrameGenDispatch*> frames(tuning.generated, nullptr);
        for (const FrameGenDispatch& dispatch : plan.dispatches) {
            for (std::size_t index = 0; index < frames.size(); ++index) {
                if (generates(dispatch) && (dispatch.copy_mask & (1u << index))) {
                    frames[index] = &dispatch;
                }
            }
        }
        const bool draw = std::find(frames.begin(), frames.end(), nullptr) == frames.end() &&
                          CanDraw(submission, outputs, frames.size());
        if (!draw && !EnsureOutput(view, submission, error)) {
            return false;
        }
        for (const FrameGenDispatch& dispatch : plan.dispatches) {
            if (draw && generates(dispatch)) {
                continue;
            }
            if (descriptor_cursor_ + kDescriptorsPerDispatch > descriptor_end_) {
                error = "D3D12 Frame Generation descriptor budget exhausted";
                return false;
            }
            const UINT base = descriptor_cursor_;
            descriptor_cursor_ += kDescriptorsPerDispatch;
            for (std::size_t i = 0; i < 4; ++i) {
                Texture* texture = Resolve(view, dispatch.inputs[i]);
                D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
                srv.Format = texture ? texture->view_format : DXGI_FORMAT_R8G8B8A8_UNORM;
                srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                srv.Texture2D.MipLevels = 1;
                device_->CreateShaderResourceView(texture ? texture->resource.Get() : nullptr, &srv, Cpu(base + static_cast<UINT>(i)));
                if (texture) Require(barriers, *texture, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            }
            for (std::size_t i = 0; i < 2; ++i) {
                Texture* texture = Resolve(view, dispatch.outputs[i]);
                D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
                uav.Format = texture ? texture->view_format : DXGI_FORMAT_R8G8B8A8_UNORM;
                uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                device_->CreateUnorderedAccessView(texture ? texture->resource.Get() : nullptr, nullptr, &uav,
                                                   Cpu(base + 4 + static_cast<UINT>(i)));
                if (texture) Require(barriers, *texture, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            }
            Flush(barriers);
            commands_->SetPipelineState(pipelines_[static_cast<std::size_t>(dispatch.pass)].Get());
            commands_->SetComputeRoot32BitConstants(0, sizeof(FrameGenConstants) / 4, &dispatch.constants, 0);
            commands_->SetComputeRootDescriptorTable(1, Gpu(base));
            commands_->SetComputeRootDescriptorTable(2, Gpu(base + 4));
            commands_->Dispatch((dispatch.width + kFrameGenThreadGroupSize - 1) / kFrameGenThreadGroupSize,
                                (dispatch.height + kFrameGenThreadGroupSize - 1) / kFrameGenThreadGroupSize, 1);
            Stamp(static_cast<int>(dispatch.pass));
            for (std::size_t index = 0; index < outputs.size(); ++index) {
                if (!(dispatch.copy_mask & (1u << index))) {
                    continue;
                }
                ID3D12Resource* output = outputs[index];
                const FrameGenImage& image = submission.outputs[index];
                const D3D12_RESOURCE_DESC output_desc = output->GetDesc();
                Require(barriers, view.output, D3D12_RESOURCE_STATE_COPY_SOURCE);
                Transition(barriers, output, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST);
                Flush(barriers);
                D3D12_TEXTURE_COPY_LOCATION out_destination{output, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
                out_destination.SubresourceIndex = image.array_index * output_desc.MipLevels;
                D3D12_TEXTURE_COPY_LOCATION out_source{view.output.resource.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
                out_source.SubresourceIndex = 0;
                commands_->CopyTextureRegion(&out_destination, static_cast<UINT>(image.rect.offset.x),
                                             static_cast<UINT>(image.rect.offset.y), 0, &out_source, nullptr);
                Transition(barriers, output, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET);
                Flush(barriers);
                Stamp(kProfileOutputCopy);
            }
        }
        if (draw && !RecordDraw(view, submission, outputs, frames, error)) {
            return false;
        }
        synthesized = plan.synthesize;
        return true;
    }

    // Swapchain images are in RENDER_TARGET whenever the runtime hands them
    // out or the application releases them (XR_KHR_D3D12_enable).
    void CopySource(ID3D12GraphicsCommandList* commands, std::vector<D3D12_RESOURCE_BARRIER>& barriers,
                    ViewResources& view, const FrameGenImage& image, ID3D12Resource* source) {
        Texture& history = view.history[view.history_state.Newest()];
        const D3D12_RESOURCE_DESC source_desc = source->GetDesc();
        Transition(barriers, source, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Require(barriers, history, D3D12_RESOURCE_STATE_COPY_DEST);
        if (!barriers.empty()) {
            commands->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
            barriers.clear();
        }
        D3D12_TEXTURE_COPY_LOCATION destination{history.resource.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        destination.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION source_location{source, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        source_location.SubresourceIndex = image.array_index * source_desc.MipLevels;
        const auto& rect = image.rect;
        const D3D12_BOX box{static_cast<UINT>(rect.offset.x), static_cast<UINT>(rect.offset.y), 0,
                            static_cast<UINT>(rect.offset.x + rect.extent.width),
                            static_cast<UINT>(rect.offset.y + rect.extent.height), 1};
        commands->CopyTextureRegion(&destination, 0, 0, 0, &source_location, &box);
        Transition(barriers, source, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
        commands->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
        barriers.clear();
    }

    D3D12_CPU_DESCRIPTOR_HANDLE Cpu(UINT index) const {
        D3D12_CPU_DESCRIPTOR_HANDLE handle = heap_->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(index) * descriptor_size_;
        return handle;
    }

    D3D12_GPU_DESCRIPTOR_HANDLE Gpu(UINT index) const {
        D3D12_GPU_DESCRIPTOR_HANDLE handle = heap_->GetGPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<UINT64>(index) * descriptor_size_;
        return handle;
    }

    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<ID3D12RootSignature> root_signature_;
    std::array<ComPtr<ID3D12PipelineState>, kFrameGenPassCount> pipelines_;
    ComPtr<ID3D12DescriptorHeap> heap_;
    UINT descriptor_size_{0};
    UINT descriptor_cursor_{0};
    UINT descriptor_end_{0};
    ComPtr<ID3D12GraphicsCommandList> commands_;
    std::array<FrameSlot, kFramesInFlight> slots_;
    ComPtr<ID3D12GraphicsCommandList> capture_commands_;
    std::array<CaptureSlot, kCaptureSlots> capture_slots_; // views x frames in flight, color and depth
    Texture check_scores_;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT check_footprint_{};
    UINT64 check_bytes_{0};
    FrameGenDepthValidator validator_;
    // Generated frames drawn straight into the output images.
    static constexpr UINT kRenderTargetViews = 16;
    bool direct_output_{true};
    ComPtr<ID3D12DescriptorHeap> rtv_heap_;
    UINT rtv_size_{0};
    UINT rtv_cursor_{0};
    std::unordered_map<std::uint64_t, ComPtr<ID3D12PipelineState>> draw_pipelines_;
    bool profiling_{false};
    ComPtr<ID3D12QueryHeap> profile_heap_;
    ComPtr<ID3D12Resource> profile_readback_;
    std::vector<int> profile_labels_; // category of the work before each stamp after the first
    std::vector<double> profile_ms_;
    UINT64 capture_count_{0};
    ComPtr<ID3D12Fence> fence_;
    UINT64 fence_value_{0};
    UINT64 submission_{0};
    HANDLE event_{nullptr};
    bool signal_failed_{false};
    ComPtr<ID3D12QueryHeap> query_heap_;
    ComPtr<ID3D12Resource> readback_;
    UINT64 timestamp_frequency_{0};
    double last_gpu_ms_{-1.0};
    std::unordered_map<XrSwapchain, std::vector<ID3D12Resource*>> images_;
    std::vector<ViewResources> views_;
};

} // namespace

std::unique_ptr<FrameGenGpu> CreateFrameGenD3D12(const void* binding, std::string& error) {
    const auto& d3d12 = *static_cast<const XrGraphicsBindingD3D12KHR*>(binding);
    if (!d3d12.device || !d3d12.queue || d3d12.queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) {
        error = "Invalid D3D12 device or direct queue";
        return nullptr;
    }
    auto gpu = std::make_unique<D3D12FrameGen>(d3d12);
    if (!gpu->Initialize(error)) {
        return nullptr;
    }
    return gpu;
}

} // namespace depthxr
