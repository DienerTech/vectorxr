#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <vulkan/vulkan_core.h>
#include <openxr/openxr_platform.h>

#include "depthxr/framegen_gpu.h"

#include <array>
#include <unordered_map>

namespace depthxr {
namespace {
using Microsoft::WRL::ComPtr;

constexpr std::size_t kFramesInFlight = 3;
constexpr UINT kDescriptorsPerFrame = 1024;
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
};

struct FrameSlot {
    ComPtr<ID3D12CommandAllocator> allocator;
    UINT64 fence_value{0};
    bool timing_pending{false};
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
        D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kDescriptorsPerFrame * kFramesInFlight,
                                        D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
        if (!check(device_->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&heap_)), "D3D12 Frame Generation descriptor heap")) {
            return false;
        }
        descriptor_size_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
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
        if (!view.output.resource || !view.history_state.Matches(request.extent, resource->GetDesc().Format, request.output,
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

    bool Execute(const std::vector<FrameGenViewSubmission>& views, const FrameGenTuning& tuning,
                 FrameGenExecuteResult& result, std::string& error) override {
        result.synthesized.assign(views.size(), false);
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

        if (views_.size() < views.size()) {
            views_.resize(views.size());
        }
        bool ok = true;
        for (std::size_t i = 0; i < views.size() && ok; ++i) {
            bool synthesized = false;
            ok = RecordView(views[i], tuning, views_[i], synthesized, error);
            result.synthesized[i] = synthesized;
        }
        if (timed) {
            commands_->EndQuery(query_heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, static_cast<UINT>(slot_index * 2 + 1));
            commands_->ResolveQueryData(query_heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, static_cast<UINT>(slot_index * 2), 2,
                                        readback_.Get(), slot_index * 2 * sizeof(UINT64));
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
        if (change & FrameGenHistoryState::kAll || !view.output.resource) {
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
            const DXGI_FORMAT intermediate = submission.request.output == FrameGenOutputKind::kFloat16
                ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
            if (!Create(view.output, extent, intermediate, intermediate, true, error)) return false;
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
                    bool& synthesized, std::string& error) {
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
        }

        const FrameGenViewPlan plan = BuildFrameGenViewPlan(view.history_state.Effective(submission.request), tuning);
        for (const FrameGenDispatch& dispatch : plan.dispatches) {
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
            }
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
    std::array<CaptureSlot, 8> capture_slots_; // views x frames in flight
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
