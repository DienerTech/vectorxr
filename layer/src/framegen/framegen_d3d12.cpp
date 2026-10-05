#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <vulkan/vulkan_core.h>
#include <openxr/openxr_platform.h>

#include "depthxr/framegen_gpu.h"

#ifdef DEPTHXR_NVOF
#include <nvOpticalFlowD3D12.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <thread>
#include <unordered_map>

namespace depthxr {
namespace {
using Microsoft::WRL::ComPtr;

constexpr std::size_t kFramesInFlight = 3;
constexpr UINT kDescriptorsPerFrame = 1024;
constexpr UINT kCaptureSlots = 8;
// A dispatch's descriptors: sampled textures t0-t4, then storage textures
// u0-u1. Depth captures run outside a frame's descriptor range: one
// dispatch per capture slot, after every frame's range.
constexpr UINT kShaderResources = 5;
constexpr UINT kFirstUav = kShaderResources;
constexpr UINT kDescriptorsPerDispatch = kShaderResources + 2;

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
    // Depth trust scores (ping-pong with the history) and the eye-shift map,
    // at half resolution; created once depth is first used.
    std::array<Texture, 2> trust;
    Texture shift_keys;
    Texture shift_map;
    bool trust_stereo{false}; // this frame's stereo evidence is in trust[newest]
    bool depth_ready{false};  // captured since the last Execute
    bool depth_fresh{false};  // captured for this Execute's frame (for the check)
    std::uint32_t depth_generation{0}; // FrameGenDepthValidator::Generation() the capture decoded with
    FrameGenDepthEncoding depth_encoding; // as the game describes it
    // Optical flow hardware (FrameGenTuning::hardware_flow): its inputs,
    // its vectors and its session for this view.
    Texture hardware_newest;
    Texture hardware_previous;
    Texture hardware_flow;
    // Matching frames as rendered, started at capture (kFlowPackRaw,
    // FrameGenViewRequest::hardware_raw): two raw luma images used in turn
    // and the vectors between them, apart from the images Execute packs.
    std::array<Texture, 2> raw;
    Texture raw_flow;
#ifdef DEPTHXR_NVOF
    NvOFHandle hardware{};
    // newest, previous, vectors, then raw[0], raw[1], raw vectors
    NvOFGPUBufferHandle hardware_buffers[6]{};
    NV_OF_PERF_LEVEL hardware_perf{NV_OF_PERF_LEVEL_UNDEFINED};
#endif
    // Each session signals its own fence: sessions finish in their own
    // order, and a fence two of them signalled could step back and leave a
    // wait for the larger value forever. Kept for the process like the
    // images (see RetireHardware).
    ComPtr<ID3D12Fence> hardware_fence;
    UINT64 hardware_fence_value{0};
    std::uint8_t raw_next{0};          // the raw image the next capture writes
    bool raw_previous{false};          // the other one holds the last capture's luma
    std::uint64_t raw_count{0};        // raw captures so far
    std::uint64_t raw_captured{0};     // the count of the capture Execute may use next (0: none)
    std::uint64_t raw_executed{0};     // the count of the newest frame the last Execute generated from
    std::uint64_t raw_reference{0};    // the count of the reference the pending raw match used
    UINT64 raw_value{0};               // the hardware fence value of the pending raw match
    UINT64 raw_last_match{0};          // ... of the last raw match, which reads both raw images
    CaptureKey raw_capture;            // the capture it matched
    bool raw_in_use{false};            // this Execute unpacks raw_flow
    int hardware_mode{0};              // what the session matched last: 1 packed, 2 raw (hints)
    FrameGenExtent hardware_extent{};
    bool hardware_fresh{false}; // no vectors of its own to hint the next match yet
    bool hardware_matched{false}; // its session has matched its packed images once (see StartHardwareWatchdog)
    bool raw_matched{false};      // ... and its raw images
};

// One view's recording: a plan recorded up to `next`, resumable after the
// optical flow hardware.
struct ViewRecording {
    const FrameGenViewSubmission* submission{nullptr};
    ViewResources* view{nullptr};
    FrameGenViewPlan plan;
    std::vector<ID3D12Resource*> outputs;
    std::vector<const FrameGenDispatch*> frames; // the dispatch writing each output
    bool draw{false};
    bool temporal{false};
    std::size_t next{0};
    bool waits_for_hardware{false}; // stopped before kFlowUnpack
};

struct FrameSlot {
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12CommandAllocator> after_hardware; // the work after the optical flow hardware
    UINT64 fence_value{0};
    bool timing_pending{false};
    // The queue's GPU and CPU (QueryPerformanceCounter) clocks just before
    // the submission, and its FrameGenExecuteResult::submission.
    UINT64 calibration_gpu{0};
    UINT64 calibration_cpu{0};
    std::uint64_t timed_submission{0};
    ComPtr<ID3D12Resource> check_readback; // depth check scores, read once the slot's fence passes
    bool check_pending{false};
    std::uint32_t check_generation{0}; // FrameGenDepthValidator::CheckGeneration() when recorded
};

struct CaptureSlot {
    ComPtr<ID3D12CommandAllocator> allocator;
    UINT64 fence_value{0};
};

// What the optical flow hardware's watchdog watches, per view (up to
// kWatchedViews): the view's fence and its latest operations, each with the
// fence value it signals when done, the fence_ value its input is ready at
// and how long it may take from then. Each operation keeps its own input:
// a later capture queued behind a stalled one never becomes ready itself.
constexpr std::size_t kWatchedViews = 4;
constexpr std::size_t kWatchedOperations = 4;
struct HardwareWatch {
    struct Operation {
        std::atomic<UINT64> done{0}; // 0 while being written
        std::atomic<UINT64> input{0};
        std::atomic<ULONGLONG> allowed{250};
    };
    std::atomic<ID3D12Fence*> fence{nullptr};
    std::array<Operation, kWatchedOperations> operations;

    void Watch(UINT64 done, UINT64 input, ULONGLONG allowed) {
        Operation& operation = operations[done % kWatchedOperations];
        operation.done = 0;
        operation.input = input;
        operation.allowed = allowed;
        operation.done = done;
    }
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
        ranges[0] = {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, kShaderResources, 0, 0, 0};
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
                       "D3D12 Frame Generation allocator") ||
                !check(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&slot.after_hardware)),
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
        static_cast<void>(quality); // the flow level does not affect the history images
        if (!view.depth.resource || !view.history_state.MatchesImages(request.extent, resource->GetDesc().Format,
                                                                      request.output)) {
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
        const bool raw = RecordRawPack(view, request, static_cast<std::size_t>(capture_count_ % capture_slots_.size()));
        HardwareWatch* watch = index < hardware_watch_.size() ? &hardware_watch_[index] : nullptr;
        hr = capture_commands_->Close();
        if (FAILED(hr)) {
            // Nothing was submitted, so tracked states no longer match the GPU:
            // force a full rebuild, which waits for the GPU before freeing.
            error = Failure("D3D12 Frame Generation capture close", hr);
            view.history_state = {};
            return false;
        }
        if (raw && view.raw_last_match && view.hardware_fence->GetCompletedValue() < view.raw_last_match) {
            // The raw image written now was the reference of this view's
            // last match (a frame ago: normally long done).
            queue_->Wait(view.hardware_fence.Get(), view.raw_last_match);
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
        view.raw_captured = 0;
        if (raw) {
            MatchRaw(view, watch);
        }
        return true;
    }

    // With optical flow hardware in use, records the captured frame's raw
    // luma (kFlowPackRaw) into the view's next raw image on the capture list.
    bool RecordRawPack(ViewResources& view, const FrameGenViewRequest& request, std::size_t slot_index) {
#ifdef DEPTHXR_NVOF
        if (!hardware_active_ || hardware_failed_ || !view.hardware || !view.raw_flow.resource) {
            return false;
        }
        const FrameGenDispatch dispatch = BuildFrameGenRawPackDispatch(request.extent);
        const UINT base = kDescriptorsPerFrame * kFramesInFlight + static_cast<UINT>(slot_index) * kDescriptorsPerDispatch;
        Texture& color = view.history[view.history_state.Newest()];
        Texture& target = view.raw[view.raw_next];
        ShaderResource(&color, base, 0);
        for (UINT i = 1; i < kShaderResources; ++i) {
            ShaderResource(nullptr, base, i);
        }
        UnorderedAccess(&target, base + kFirstUav);
        UnorderedAccess(nullptr, base + kFirstUav + 1);
        std::vector<D3D12_RESOURCE_BARRIER> barriers;
        Require(barriers, color, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Require(barriers, target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        if (!barriers.empty()) {
            capture_commands_->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
            barriers.clear();
        }
        ID3D12DescriptorHeap* heaps[] = {heap_.Get()};
        capture_commands_->SetDescriptorHeaps(1, heaps);
        capture_commands_->SetComputeRootSignature(root_signature_.Get());
        capture_commands_->SetPipelineState(pipelines_[static_cast<std::size_t>(FrameGenPass::kFlowPackRaw)].Get());
        capture_commands_->SetComputeRoot32BitConstants(0, sizeof(FrameGenConstants) / 4, &dispatch.constants, 0);
        capture_commands_->SetComputeRootDescriptorTable(1, Gpu(base));
        capture_commands_->SetComputeRootDescriptorTable(2, Gpu(base + kFirstUav));
        capture_commands_->Dispatch((dispatch.width + kFrameGenThreadGroupSize - 1) / kFrameGenThreadGroupSize,
                                    (dispatch.height + kFrameGenThreadGroupSize - 1) / kFrameGenThreadGroupSize, 1);
        // The hardware reads and writes its images in COMMON.
        Require(barriers, target, D3D12_RESOURCE_STATE_COMMON);
        Require(barriers, view.raw[view.raw_next ^ 1u], D3D12_RESOURCE_STATE_COMMON);
        Require(barriers, view.raw_flow, D3D12_RESOURCE_STATE_COMMON);
        capture_commands_->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
        return true;
#else
        static_cast<void>(view);
        static_cast<void>(request);
        static_cast<void>(slot_index);
        return false;
#endif
    }

    // After the raw pack is submitted: has the hardware match it against the
    // view's previous raw image, if the previous capture packed one.
    void MatchRaw(ViewResources& view, HardwareWatch* watch) {
#ifdef DEPTHXR_NVOF
        const std::uint8_t newest = view.raw_next;
        view.raw_next ^= 1u;
        const std::uint64_t reference = view.raw_previous ? view.raw_count : 0;
        view.raw_captured = ++view.raw_count;
        view.raw_previous = true;
        view.raw_value = 0;
        if (!reference) {
            return;
        }
        NV_OF_FENCE_POINT ready{fence_.Get(), fence_value_};
        NV_OF_EXECUTE_INPUT_PARAMS_D3D12 in{};
        in.inputFrame = view.hardware_buffers[3 + newest];
        in.referenceFrame = view.hardware_buffers[3 + (newest ^ 1u)];
        // Its own hints only from a raw match of the frame before.
        in.disableTemporalHints = view.hardware_mode == 2 && view.raw_executed == reference ? NV_OF_FALSE : NV_OF_TRUE;
        in.numFencePoints = 1;
        in.fencePoint = &ready;
        NV_OF_FENCE_POINT done{view.hardware_fence.Get(), ++view.hardware_fence_value};
        NV_OF_EXECUTE_OUTPUT_PARAMS_D3D12 out{};
        out.outputBuffer = view.hardware_buffers[5];
        out.fencePoint = &done;
        if (watch) {
            // The first match on the raw images may take long too (see
            // StartHardwareWatchdog).
            watch->Watch(done.value, ready.value, view.raw_matched ? 250 : 3000);
        }
        if (stall_hardware_for_test_) {
            stall_hardware_for_test_ = false;
        } else if (hardware_api_.nvOFExecuteD3D12(view.hardware, &in, &out) != NV_OF_SUCCESS) {
            HardwareFailed("the optical flow hardware failed: " + HardwareError(view.hardware));
            queue_->Signal(view.hardware_fence.Get(), view.hardware_fence_value);
        }
        view.raw_matched = true;
        view.hardware_mode = 2;
        view.raw_reference = reference;
        view.raw_value = view.hardware_fence_value;
        view.raw_capture = view.capture;
        view.raw_last_match = view.hardware_fence_value;
#else
        static_cast<void>(view);
        static_cast<void>(watch);
#endif
    }

    bool WaitIdle() override {
        if (!WaitFence(fence_value_, 2000)) {
            return false;
        }
        // The optical flow hardware signals its own fences.
        const ULONGLONG deadline = GetTickCount64() + 2000;
        for (const ViewResources& view : views_) {
            while (view.hardware_fence && view.hardware_fence->GetCompletedValue() < view.hardware_fence_value) {
                if (GetTickCount64() >= deadline) {
                    return false;
                }
                Sleep(1);
            }
        }
        return true;
    }

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
        for (UINT i = 1; i < kShaderResources; ++i) {
            ShaderResource(nullptr, base, i);
        }
        UnorderedAccess(&view.depth, base + kFirstUav);
        UnorderedAccess(nullptr, base + kFirstUav + 1);

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
        depth_encoding.scale = validator_.Scale();
        const FrameGenDispatch dispatch = BuildFrameGenDepthDispatch(
            request.extent, depth.rect.offset.x, depth.rect.offset.y, static_cast<std::uint32_t>(depth.rect.extent.width),
            static_cast<std::uint32_t>(depth.rect.extent.height), depth.array_index, depth_encoding);
        capture_commands_->SetPipelineState(pipelines_[static_cast<std::size_t>(FrameGenPass::kDepth)].Get());
        capture_commands_->SetComputeRoot32BitConstants(0, sizeof(FrameGenConstants) / 4, &dispatch.constants, 0);
        capture_commands_->SetComputeRootDescriptorTable(1, Gpu(base));
        capture_commands_->SetComputeRootDescriptorTable(2, Gpu(base + kFirstUav));
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
        return {validator_.Reading(), validator_.Decisions(), validator_.Evidence(), validator_.Scale()};
    }

    // Records what `other` says about `view`'s depth (see FG_PASS_DEPTH_TRUST)
    // into the view's newest trust texture, for this frame's kTrustUpdate;
    // both views' newest images must be this frame's.
    bool RecordDepthTrust(const FrameGenViewSubmission& submission, const FrameGenViewSubmission& other_submission,
                          ViewResources& view, ViewResources& other, std::string& error) {
        if (descriptor_cursor_ + kDescriptorsPerDispatch > descriptor_end_) {
            error = "D3D12 Frame Generation descriptor budget exhausted";
            return false;
        }
        if (!EnsureTrust(view, submission.request.extent, error)) {
            return false;
        }
        const UINT base = descriptor_cursor_;
        descriptor_cursor_ += kDescriptorsPerDispatch;
        Texture* inputs[kShaderResources] = {&view.history[view.history_state.Newest()],
                                             &other.history[other.history_state.Newest()], &view.depth, nullptr, nullptr};
        std::vector<D3D12_RESOURCE_BARRIER> barriers;
        for (UINT i = 0; i < kShaderResources; ++i) {
            ShaderResource(inputs[i], base, i);
            if (inputs[i]) Require(barriers, *inputs[i], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        Texture& evidence = view.trust[view.history_state.Newest()];
        UnorderedAccess(&evidence, base + kFirstUav);
        UnorderedAccess(nullptr, base + kFirstUav + 1);
        Require(barriers, evidence, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Flush(barriers);
        const FrameGenDispatch dispatch = BuildFrameGenDepthTrustDispatch(submission.request.extent, submission.request.newest,
                                                                          other_submission.request.newest);
        commands_->SetPipelineState(pipelines_[static_cast<std::size_t>(FrameGenPass::kDepthTrust)].Get());
        commands_->SetComputeRoot32BitConstants(0, sizeof(FrameGenConstants) / 4, &dispatch.constants, 0);
        commands_->SetComputeRootDescriptorTable(1, Gpu(base));
        commands_->SetComputeRootDescriptorTable(2, Gpu(base + kFirstUav));
        commands_->Dispatch((dispatch.width + kFrameGenThreadGroupSize - 1) / kFrameGenThreadGroupSize,
                            (dispatch.height + kFrameGenThreadGroupSize - 1) / kFrameGenThreadGroupSize, 1);
        view.trust_stereo = true;
        return true;
    }

    // A view's depth trust and eye-shift textures, created the first time
    // its depth is used.
    bool EnsureTrust(ViewResources& view, FrameGenExtent extent, std::string& error) {
        if (view.shift_map.resource) {
            return true;
        }
        const FrameGenExtent half = FrameGenPyramid(extent)[1];
        for (Texture& trust : view.trust) {
            if (!Create(trust, half, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT, true, error)) return false;
        }
        return Create(view.shift_keys, half, DXGI_FORMAT_R32_UINT, DXGI_FORMAT_R32_UINT, true, error) &&
               Create(view.shift_map, half, DXGI_FORMAT_R32_UINT, DXGI_FORMAT_R32_UINT, true, error);
    }

    // A 2D shader resource view of `texture` (a null one without) for
    // register t`slot` of the dispatch whose descriptors start at `base`.
    // The fifth is read as unsigned integers.
    void ShaderResource(Texture* texture, UINT base, UINT slot) {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = texture ? texture->view_format : slot == 4 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R8G8B8A8_UNORM;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        device_->CreateShaderResourceView(texture ? texture->resource.Get() : nullptr, &srv, Cpu(base + slot));
    }

    void UnorderedAccess(Texture* texture, UINT index) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.Format = texture ? texture->view_format : DXGI_FORMAT_R8G8B8A8_UNORM;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        device_->CreateUnorderedAccessView(texture ? texture->resource.Get() : nullptr, nullptr, &uav, Cpu(index));
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
        Texture* inputs[kShaderResources] = {&view0.history[view0.history_state.Newest()],
                                             &view1.history[view1.history_state.Newest()], &view0.depth, nullptr, nullptr};
        std::vector<D3D12_RESOURCE_BARRIER> barriers;
        for (UINT i = 0; i < kShaderResources; ++i) {
            ShaderResource(inputs[i], base, i);
            if (inputs[i]) Require(barriers, *inputs[i], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        UnorderedAccess(&check_scores_, base + kFirstUav);
        UnorderedAccess(nullptr, base + kFirstUav + 1);
        Require(barriers, check_scores_, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Flush(barriers);
        const FrameGenDispatch dispatch =
            BuildFrameGenDepthCheckDispatch(first.request.newest, second.request.newest, view0.depth_encoding,
                                            validator_.Scale(), validator_.ProbeStep());
        commands_->SetPipelineState(pipelines_[static_cast<std::size_t>(FrameGenPass::kDepthCheck)].Get());
        commands_->SetComputeRoot32BitConstants(0, sizeof(FrameGenConstants) / 4, &dispatch.constants, 0);
        commands_->SetComputeRootDescriptorTable(1, Gpu(base));
        commands_->SetComputeRootDescriptorTable(2, Gpu(base + kFirstUav));
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
        slot.check_generation = validator_.CheckGeneration();
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
                } else if (reading == 2) {
                    evidence.binocular += texel[0];
                    evidence.cockpit += texel[1];
                } else if (reading == 3) {
                    for (int probe = 0; probe < 4; ++probe) evidence.probes[probe] += texel[probe];
                } else {
                    evidence.probes[4] += texel[0];
                    evidence.probed += texel[1];
                }
            }
        }
        const D3D12_RANGE written{0, 0};
        slot.check_readback->Unmap(0, &written);
        // A check of depth decoded before the validator last changed the
        // decoding, or probed with another step, scored other readings.
        if (slot.check_generation == validator_.CheckGeneration()) {
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

    bool ReadFlow(std::size_t index, std::vector<float>& xy, FrameGenExtent& cells) override {
        if (index >= views_.size() || !WaitIdle()) {
            return false;
        }
        ViewResources& view = views_[index];
        // The pair flipped after the Execute that wrote it.
        Texture& flow = view.filtered[view.history_state.Previous()];
        if (!flow.resource) {
            return false;
        }
        const D3D12_RESOURCE_DESC desc = flow.resource->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT64 bytes = 0;
        device_->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
        D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_READBACK};
        D3D12_RESOURCE_DESC buffer{};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = bytes;
        buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
        buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> readback;
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> list;
        if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST,
                                                    nullptr, IID_PPV_ARGS(&readback))) ||
            FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
            FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                              IID_PPV_ARGS(&list)))) {
            return false;
        }
        std::vector<D3D12_RESOURCE_BARRIER> barriers;
        Require(barriers, flow, D3D12_RESOURCE_STATE_COPY_SOURCE);
        if (!barriers.empty()) {
            list->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
        }
        D3D12_TEXTURE_COPY_LOCATION destination{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        destination.PlacedFootprint = footprint;
        D3D12_TEXTURE_COPY_LOCATION source{flow.resource.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        if (FAILED(list->Close())) {
            return false;
        }
        ID3D12CommandList* lists[] = {list.Get()};
        queue_->ExecuteCommandLists(1, lists);
        if (FAILED(queue_->Signal(fence_.Get(), ++fence_value_)) || !WaitIdle()) {
            return false;
        }
        void* mapped = nullptr;
        if (FAILED(readback->Map(0, nullptr, &mapped))) {
            return false;
        }
        auto half = [](std::uint16_t h) {
            const std::uint32_t sign = (h & 0x8000u) << 16, exponent = (h >> 10) & 0x1fu, mantissa = h & 0x3ffu;
            std::uint32_t bits;
            if (exponent == 0) {
                // Zero or subnormal.
                float value = std::ldexp(static_cast<float>(mantissa), -24);
                return sign ? -value : value;
            } else if (exponent == 31) {
                bits = sign | 0x7f800000u | (mantissa << 13);
            } else {
                bits = sign | ((exponent + 112) << 23) | (mantissa << 13);
            }
            float value;
            std::memcpy(&value, &bits, sizeof(value));
            return value;
        };
        const FrameGenExtent extent = view.history_state.Extent();
        cells = {static_cast<std::uint32_t>(desc.Width), desc.Height};
        xy.resize(std::size_t{cells.width} * cells.height * 2);
        for (std::uint32_t y = 0; y < cells.height; ++y) {
            const auto* row = reinterpret_cast<const std::uint16_t*>(static_cast<const std::byte*>(mapped) +
                                                                      footprint.Offset + y * footprint.Footprint.RowPitch);
            for (std::uint32_t x = 0; x < cells.width; ++x) {
                xy[(std::size_t{y} * cells.width + x) * 2] = half(row[x * 4]) * extent.width;
                xy[(std::size_t{y} * cells.width + x) * 2 + 1] = half(row[x * 4 + 1]) * extent.height;
            }
        }
        readback->Unmap(0, nullptr);
        return true;
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
        for (UINT i = 0; i < kShaderResources; ++i) {
            Texture* texture = Resolve(view, first.inputs[i]);
            ShaderResource(texture, base, i);
            if (texture) Require(barriers, *texture, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
        }
        UnorderedAccess(nullptr, base + kFirstUav);
        UnorderedAccess(nullptr, base + kFirstUav + 1);
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
            std::memcpy(i < 3 ? &constants.size[1 + i] : &constants.options[0], &fraction, sizeof(fraction));
        }
        const D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(submission.request.extent.width),
                                      static_cast<float>(submission.request.extent.height), 0.0f, 1.0f};
        const D3D12_RECT scissor{0, 0, static_cast<LONG>(submission.request.extent.width),
                                 static_cast<LONG>(submission.request.extent.height)};
        commands_->SetGraphicsRootSignature(root_signature_.Get());
        commands_->SetPipelineState(pipeline);
        commands_->SetGraphicsRoot32BitConstants(0, sizeof(FrameGenConstants) / 4, &constants, 0);
        commands_->SetGraphicsRootDescriptorTable(1, Gpu(base));
        commands_->SetGraphicsRootDescriptorTable(2, Gpu(base + kFirstUav));
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
    static constexpr int kProfileHardware = kProfileOutputCopy + 1; // waiting for the optical flow hardware
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
        profile_ms_.assign(kProfileHardware + 1, 0.0);
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
        if (hardware_stuck_.exchange(false)) {
            HardwareFailed("the optical flow hardware stopped responding");
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
        FrameGenTuning effective_tuning = tuning;
        effective_tuning.hardware_flow = tuning.hardware_flow && HardwareFlowReady();
        for (std::size_t i = 0; i < views.size() && effective_tuning.hardware_flow; ++i) {
            // A session per view: its hints follow that view's motion.
            effective_tuning.hardware_flow = EnsureHardware(views_[i], i, views[i], tuning);
        }
        // Captures start matching on the hardware while it is in use.
        hardware_active_ = effective_tuning.hardware_flow;
        bool ok = true;
        // Depth the other eye disproves is dropped per texel before any view
        // uses it. Needs both eyes' images of this frame, captured at release.
        if (views.size() >= 2 && validator_.Usable()) {
            bool both = true;
            for (std::size_t i = 0; i < 2; ++i) {
                both = both && Prepared(views[i], views_[i]) && views_[i].capture.Matches(views[i].source);
            }
            for (std::size_t i = 0; i < 2 && both && ok; ++i) {
                ViewResources& view = views_[i];
                if (views[i].request.depth && view.depth_ready && view.depth_generation == validator_.Generation()) {
                    ok = RecordDepthTrust(views[i], views[1 - i], view, views_[1 - i], error);
                    Stamp(static_cast<int>(FrameGenPass::kDepthTrust));
                }
            }
        }
        // Every view up to the optical flow hardware, the hardware for all of
        // them, then the rest (without hardware flow, everything at once).
        std::vector<ViewRecording> recordings(views.size());
        bool hardware = false;
        for (std::size_t i = 0; i < views.size() && ok; ++i) {
            bool synthesized = false;
            bool depth = false;
            ok = BeginView(views[i], effective_tuning, views_[i], recordings[i], synthesized, depth, error);
            result.synthesized[i] = synthesized;
            result.depth[i] = depth;
        }
        // Research dumps read the inputs before the plans change the depth.
        std::vector<DumpCopy> dump;
        if (ok && !dump_directory_.empty()) {
            ok = RecordDump(views, dump, error);
        }
        for (std::size_t i = 0; i < views.size() && ok; ++i) {
            ok = RecordPlan(recordings[i], effective_tuning.hardware_flow, error);
            hardware = hardware || recordings[i].waits_for_hardware;
        }
        if (ok && hardware) {
            ok = RunHardwareFlow(recordings, slot, error);
            for (std::size_t i = 0; i < views.size() && ok; ++i) {
                ok = RecordPlan(recordings[i], false, error);
            }
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
                    RetireHardware(views_[i]);
                    views_[i] = {};
                } else {
                    views_[i].history_state = {};
                }
            }
            return false;
        }
        slot.timed_submission = 0;
        if (timed && SUCCEEDED(queue_->GetClockCalibration(&slot.calibration_gpu, &slot.calibration_cpu))) {
            slot.timed_submission = submission_ + 1;
        }
        result.submission = slot.timed_submission;
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
        if (!dump.empty()) {
            WriteDump(views, effective_tuning, result, dump);
        }
        dump_directory_.clear();
        for (std::size_t i = 0; i < views.size(); ++i) {
            views_[i].history_state.Commit(result.synthesized[i], result.depth[i]);
            views_[i].capture.valid = false;
        }
        return true;
    }

    bool DumpNextExecute(const std::string& directory) override {
        dump_directory_ = directory;
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
                view.depth.resource.Detach();
                for (auto& texture : view.trust) texture.resource.Detach();
                view.shift_keys.resource.Detach();
                view.shift_map.resource.Detach();
            }
        }
        StopHardwareWatchdog();
        for (auto& view : views_) {
            RetireHardware(view);
        }
        hardware_loaded_ = false;
        views_.clear();
        images_.clear();
        if (event_) {
            CloseHandle(event_);
            event_ = nullptr;
        }
    }

    double LastGpuMilliseconds() const override { return last_gpu_ms_; }
    std::optional<FrameGenGpuWindow> LastGpuWindow() const override { return last_window_; }

  private:
    // A texture copied into a readback buffer for a research dump.
    struct DumpCopy {
        std::string name;
        ComPtr<ID3D12Resource> buffer;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT rows{0};
        UINT64 row_bytes{0};
        DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN};
    };

    // Copies every view's newest image and depth into readback buffers.
    bool RecordDump(const std::vector<FrameGenViewSubmission>& views, std::vector<DumpCopy>& dump, std::string& error) {
        std::vector<D3D12_RESOURCE_BARRIER> barriers;
        auto copy = [&](Texture& texture, const std::string& name) {
            if (!texture.resource) {
                return true;
            }
            DumpCopy entry;
            entry.name = name;
            const D3D12_RESOURCE_DESC desc = texture.resource->GetDesc();
            entry.format = texture.view_format;
            UINT64 bytes = 0;
            device_->GetCopyableFootprints(&desc, 0, 1, 0, &entry.footprint, &entry.rows, &entry.row_bytes, &bytes);
            D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_READBACK};
            D3D12_RESOURCE_DESC buffer{};
            buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            buffer.Width = bytes;
            buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
            buffer.SampleDesc.Count = 1;
            buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            const HRESULT hr = device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                                                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                                IID_PPV_ARGS(&entry.buffer));
            if (FAILED(hr)) {
                error = Failure("D3D12 Frame Generation dump buffer", hr);
                return false;
            }
            Require(barriers, texture, D3D12_RESOURCE_STATE_COPY_SOURCE);
            Flush(barriers);
            D3D12_TEXTURE_COPY_LOCATION destination{entry.buffer.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
            destination.PlacedFootprint = entry.footprint;
            D3D12_TEXTURE_COPY_LOCATION source{texture.resource.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
            commands_->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
            dump.push_back(std::move(entry));
            return true;
        };
        for (std::size_t i = 0; i < views.size() && i < views_.size(); ++i) {
            ViewResources& view = views_[i];
            if (!copy(view.history[view.history_state.Newest()], "view" + std::to_string(i) + "-color.raw") ||
                !copy(view.depth, "view" + std::to_string(i) + "-depth.f32")) {
                return false;
            }
        }
        return true;
    }

    // After the dumped Execute: waits for it and writes the files.
    void WriteDump(const std::vector<FrameGenViewSubmission>& views, const FrameGenTuning& tuning,
                   const FrameGenExecuteResult& result, std::vector<DumpCopy>& dump) {
        if (!WaitIdle()) {
            return;
        }
        // Hundreds of megabytes per frame: written by a thread of their own,
        // so the game only waits for this Execute to finish on the GPU.
        std::thread([entries = std::move(dump), directory = dump_directory_]() mutable {
            for (DumpCopy& entry : entries) {
                void* mapped = nullptr;
                if (FAILED(entry.buffer->Map(0, nullptr, &mapped))) {
                    continue;
                }
                std::ofstream file(directory + "\\" + entry.name, std::ios::binary);
                for (UINT row = 0; row < entry.rows && file; ++row) {
                    file.write(static_cast<const char*>(mapped) + entry.footprint.Offset +
                                   row * entry.footprint.Footprint.RowPitch,
                               static_cast<std::streamsize>(entry.row_bytes));
                }
                entry.buffer->Unmap(0, nullptr);
            }
        }).detach();
        std::ofstream meta(dump_directory_ + "\\frame.txt");
        meta.precision(9);
        meta << "mode " << static_cast<int>(tuning.mode) << "\nquality " << static_cast<int>(tuning.quality)
             << "\nhardware " << tuning.hardware_flow << "\nsuppression " << tuning.artifact_suppression
             << "\nlimit " << tuning.motion_limit_percent << "\ninterval_ratio " << tuning.interval_ratio
             << "\ngenerated " << static_cast<int>(tuning.generated) << "\nfractions";
        for (const float fraction : tuning.fractions) meta << ' ' << fraction;
        meta << "\ndepth_reading " << static_cast<int>(validator_.Reading()) << "\nviews " << views.size() << '\n';
        auto geometry = [&](const char* name, const FrameGenViewGeometry& g) {
            meta << name << ' ' << g.orientation[0] << ' ' << g.orientation[1] << ' ' << g.orientation[2] << ' '
                 << g.orientation[3] << ' ' << g.position[0] << ' ' << g.position[1] << ' ' << g.position[2] << ' '
                 << g.angle_left << ' ' << g.angle_right << ' ' << g.angle_up << ' ' << g.angle_down << '\n';
        };
        for (std::size_t i = 0; i < views.size(); ++i) {
            const FrameGenViewRequest& request = views[i].request;
            const FrameGenExtent half = FrameGenPyramid(request.extent)[1];
            meta << "view " << i << " extent " << request.extent.width << ' ' << request.extent.height << " half "
                 << half.width << ' ' << half.height << " format "
                 << (i < views_.size() ? static_cast<int>(views_[i].history[views_[i].history_state.Newest()].view_format) : 0)
                 << " output " << static_cast<int>(request.output) << " synthesize " << request.synthesize
                 << " depth " << request.depth << " used " << (i < result.depth.size() && result.depth[i])
                 << " shift " << request.shift[0] << ' ' << request.shift[1] << ' ' << request.shift[2] << '\n';
            geometry("newest", request.newest);
            geometry("previous", request.previous);
        }
    }

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
            LARGE_INTEGER frequency{};
            if (slot.timed_submission && stamps[1] >= stamps[0] && QueryPerformanceFrequency(&frequency)) {
                // GPU ticks after the calibration, as steady_clock time
                // (which counts QueryPerformanceCounter from zero).
                auto clock = [&](UINT64 stamp) {
                    const double seconds =
                        static_cast<double>(slot.calibration_cpu) / static_cast<double>(frequency.QuadPart) +
                        (static_cast<double>(stamp) - static_cast<double>(slot.calibration_gpu)) /
                            static_cast<double>(timestamp_frequency_);
                    return std::chrono::steady_clock::time_point(std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                        std::chrono::duration<double>(seconds)));
                };
                last_window_ = FrameGenGpuWindow{slot.timed_submission, clock(stamps[0]), clock(stamps[1])};
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

    // Whether EnsureView would keep the view's images (history and depth) as
    // they are.
    bool Prepared(const FrameGenViewSubmission& submission, const ViewResources& view) const {
        const auto it = images_.find(submission.source.swapchain);
        if (it == images_.end() || submission.source.image_index >= it->second.size() || !view.depth.resource) {
            return false;
        }
        return view.history_state.MatchesImages(submission.request.extent,
                                                it->second[submission.source.image_index]->GetDesc().Format,
                                                submission.request.output);
    }

    bool EnsureView(ViewResources& view, const FrameGenViewSubmission& submission, ID3D12Resource* source,
                    const FrameGenTuning& tuning, std::string& error) {
        const D3D12_RESOURCE_DESC source_desc = source->GetDesc();
        const FrameGenExtent extent = submission.request.extent;
        const std::uint8_t final_level = FrameGenFinalFlowLevel(tuning);
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
            view.trust = {};  // likewise (see EnsureTrust)
            view.shift_keys = {};
            view.shift_map = {};
            view.trust_stereo = false;
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

    // --- Optical flow hardware (NVIDIA's optical flow accelerator, RTX 20-50) ---

    // Loads the driver's optical flow API once; false (with a status) when
    // this build, the driver or the GPU cannot provide it.
    bool HardwareFlowReady() {
#ifdef DEPTHXR_NVOF
        if (hardware_failed_) {
            return false;
        }
        if (hardware_loaded_) {
            return true;
        }
        const HMODULE library = LoadLibraryW(L"nvofapi64.dll");
        const auto create = library ? reinterpret_cast<decltype(&NvOFAPICreateInstanceD3D12)>(
                                          GetProcAddress(library, "NvOFAPICreateInstanceD3D12"))
                                    : nullptr;
        if (!create) {
            return HardwareFailed("the NVIDIA driver's optical flow library (nvofapi64.dll) is not installed");
        }
        if (create(NV_OF_API_VERSION, &hardware_api_) != NV_OF_SUCCESS) {
            return HardwareFailed("the NVIDIA driver does not offer optical flow API " +
                                  std::to_string(NV_OF_API_MAJOR_VERSION) + "." + std::to_string(NV_OF_API_MINOR_VERSION));
        }
        StartHardwareWatchdog();
        hardware_loaded_ = true;
        hardware_status_ = "active";
        return true;
#else
        hardware_status_ = "this build of VectorXR has no NVIDIA optical flow support";
        return false;
#endif
    }

    std::string HardwareFlowStatus() const override { return hardware_status_; }
    void StallHardwareForTest() override { stall_hardware_for_test_ = true; }

    // The game's queue waits on the optical flow hardware's fence: should
    // the hardware stop answering, the game would hang behind it, and stop
    // calling us. A watchdog thread signals a fence a quarter second overdue
    // itself, which releases the queue; the hardware is not used again.
    // Overdue counts from when the hardware's input was ready (the game's
    // own GPU work before ours, a hitch included, is not the hardware's
    // fault). A new session's first match may take longer (the driver sets
    // up its CUDA work then), so it gets a few seconds.
    void StartHardwareWatchdog() {
        ID3D12Fence* input = fence_.Get();
        hardware_watchdog_stop_ = false;
        hardware_watchdog_ = std::thread([this, input] {
            // Per watched operation: when its input was first seen ready, for
            // which operation.
            std::array<std::array<ULONGLONG, kWatchedOperations>, kWatchedViews> ready_since{};
            std::array<std::array<UINT64, kWatchedOperations>, kWatchedViews> ready_done{};
            while (!hardware_watchdog_stop_) {
                Sleep(50);
                for (std::size_t i = 0; i < kWatchedViews; ++i) {
                    HardwareWatch& watch = hardware_watch_[i];
                    ID3D12Fence* fence = watch.fence;
                    for (std::size_t j = 0; j < kWatchedOperations; ++j) {
                        HardwareWatch::Operation& operation = watch.operations[j];
                        const UINT64 done = operation.done;
                        const UINT64 needed = operation.input;
                        const ULONGLONG allowed = operation.allowed;
                        if (!fence || !done || operation.done != done || fence->GetCompletedValue() >= done ||
                            input->GetCompletedValue() < needed) {
                            ready_since[i][j] = 0;
                            continue;
                        }
                        const ULONGLONG now = GetTickCount64();
                        if (!ready_since[i][j] || ready_done[i][j] != done) {
                            ready_since[i][j] = now;
                            ready_done[i][j] = done;
                        } else if (now - ready_since[i][j] > allowed) {
                            // Releases every queue wait up to it; the
                            // hardware is not used again, and its session
                            // and images are never freed (it may still be
                            // working on them).
                            hardware_hung_ = true;
                            fence->Signal(done);
                            hardware_stuck_ = true;
                        }
                    }
                }
            }
        });
    }

    void StopHardwareWatchdog() {
        hardware_watchdog_stop_ = true;
        if (hardware_watchdog_.joinable()) {
            hardware_watchdog_.join();
        }
    }

    bool HardwareFailed(const std::string& reason) {
        hardware_failed_ = true;
        hardware_status_ = reason;
        return false;
    }

    // The view's optical flow session and images for its size; false (and
    // hardware flow off for the session) when the hardware refuses them.
    bool EnsureHardware(ViewResources& view, std::size_t index, const FrameGenViewSubmission& submission,
                        const FrameGenTuning& tuning) {
#ifdef DEPTHXR_NVOF
        const FrameGenExtent input = FrameGenPyramid(submission.request.extent)[1];
        const FrameGenExtent flow = FrameGenPyramid(submission.request.extent)[kFrameGenHardwareFlowLevel];
        const NV_OF_PERF_LEVEL perf =
            tuning.quality == FrameGenFlowQuality::kPerformance ? NV_OF_PERF_LEVEL_FAST : NV_OF_PERF_LEVEL_MEDIUM;
        if (view.hardware && view.hardware_extent.width == input.width && view.hardware_extent.height == input.height &&
            view.hardware_perf == perf) {
            return true;
        }
        if (view.hardware && !WaitIdle()) {
            return HardwareFailed("the GPU did not finish before the optical flow session was resized");
        }
        RetireHardware(view);
        // The hardware reads and writes them in COMMON, to which the work
        // before it returns them (RunHardwareFlow).
        std::string error;
        if (!Create(view.hardware_newest, input, DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8_UNORM, true, error) ||
            !Create(view.hardware_previous, input, DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8_UNORM, true, error) ||
            !Create(view.hardware_flow, flow, DXGI_FORMAT_R16G16_SINT, DXGI_FORMAT_R16G16_SINT, false, error) ||
            !Create(view.raw[0], input, DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8_UNORM, true, error) ||
            !Create(view.raw[1], input, DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8_UNORM, true, error) ||
            !Create(view.raw_flow, flow, DXGI_FORMAT_R16G16_SINT, DXGI_FORMAT_R16G16_SINT, false, error)) {
            return HardwareFailed("its images could not be created (" + error + ")");
        }
        if (hardware_api_.nvCreateOpticalFlowD3D12(device_.Get(), &view.hardware) != NV_OF_SUCCESS || !view.hardware) {
            view.hardware = {};
            return HardwareFailed("this GPU has no optical flow hardware the driver can use");
        }
        NV_OF_INIT_PARAMS init{};
        init.width = input.width;
        init.height = input.height;
        init.outGridSize = NV_OF_OUTPUT_VECTOR_GRID_SIZE_4;
        init.hintGridSize = NV_OF_HINT_VECTOR_GRID_SIZE_UNDEFINED;
        init.mode = NV_OF_MODE_OPTICALFLOW;
        init.perfLevel = perf;
        init.predDirection = NV_OF_PRED_DIRECTION_FORWARD;
        init.inputBufferFormat = NV_OF_BUFFER_FORMAT_GRAYSCALE8;
        if (hardware_api_.nvOFInit(view.hardware, &init) != NV_OF_SUCCESS) {
            const std::string reason = "the optical flow hardware refused a " + std::to_string(input.width) + "x" +
                                       std::to_string(input.height) + " session: " + HardwareError(view.hardware);
            RetireHardware(view);
            return HardwareFailed(reason);
        }
        if (FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&view.hardware_fence)))) {
            RetireHardware(view);
            return HardwareFailed("its fence could not be created");
        }
        view.hardware_fence_value = 0;
        if (index < hardware_watch_.size()) {
            for (auto& operation : hardware_watch_[index].operations) {
                operation.done = 0;
            }
            hardware_watch_[index].fence = view.hardware_fence.Get();
        }
        Texture* images[6] = {&view.hardware_newest, &view.hardware_previous, &view.hardware_flow,
                              &view.raw[0],          &view.raw[1],               &view.raw_flow};
        for (int i = 0; i < 6; ++i) {
            NV_OF_REGISTER_RESOURCE_PARAMS_D3D12 params{};
            params.resource = images[i]->resource.Get();
            params.inputFencePoint = {view.hardware_fence.Get(), view.hardware_fence_value};
            params.hOFGpuBuffer = &view.hardware_buffers[i];
            params.outputFencePoint = {view.hardware_fence.Get(), ++view.hardware_fence_value};
            if (hardware_api_.nvOFRegisterResourceD3D12(view.hardware, &params) != NV_OF_SUCCESS) {
                const std::string reason = "the optical flow hardware refused its images: " + HardwareError(view.hardware);
                RetireHardware(view);
                return HardwareFailed(reason);
            }
        }
        view.hardware_extent = input;
        view.hardware_perf = perf;
        view.hardware_fresh = true;
        view.hardware_matched = view.raw_matched = false;
        view.hardware_mode = 0;
        view.raw_previous = false;
        view.raw_value = 0;
        view.raw_captured = view.raw_executed = 0;
        return true;
#else
        static_cast<void>(view);
        static_cast<void>(index);
        static_cast<void>(submission);
        static_cast<void>(tuning);
        return false;
#endif
    }

#ifdef DEPTHXR_NVOF
    std::string HardwareError(NvOFHandle handle) {
        char text[MIN_ERROR_STRING_SIZE * 4] = {};
        std::uint32_t size = sizeof(text);
        hardware_api_.nvOFGetLastError(handle, text, &size);
        return text;
    }
#endif

    // Ends a view's optical flow session. The driver crashes releasing
    // images (or the fence) it had registered once the session is gone, so
    // they are kept for the life of the process: a few megabytes per resize.
    void RetireHardware(ViewResources& view) {
#ifdef DEPTHXR_NVOF
        if (view.hardware && hardware_hung_) {
            // Released by the watchdog: the hardware may still read and write
            // its images, so nothing it was given is ever freed.
            view.hardware = {};
            for (NvOFGPUBufferHandle& buffer : view.hardware_buffers) {
                buffer = {};
            }
            for (Texture* texture : {&view.hardware_newest, &view.hardware_previous, &view.hardware_flow, &view.raw[0],
                                     &view.raw[1], &view.raw_flow}) {
                texture->resource.Detach();
                *texture = {};
            }
            view.hardware_fence.Detach();
            view.hardware_fence_value = 0;
        }
        if (view.hardware) {
            for (NvOFGPUBufferHandle& buffer : view.hardware_buffers) {
                if (buffer) {
                    NV_OF_UNREGISTER_RESOURCE_PARAMS_D3D12 params{buffer};
                    hardware_api_.nvOFUnregisterResourceD3D12(&params);
                    buffer = {};
                }
            }
            hardware_api_.nvOFDestroy(view.hardware);
            view.hardware = {};
            for (Texture* texture : {&view.hardware_newest, &view.hardware_previous, &view.hardware_flow, &view.raw[0],
                                     &view.raw[1], &view.raw_flow}) {
                texture->resource.Detach();
                *texture = {};
            }
            // The watchdog may still look at it: a fence that is never freed.
            view.hardware_fence.Detach();
            view.hardware_fence_value = 0;
        }
#endif
        view.hardware_extent = {};
    }

    // Submits the work recorded so far, has the optical flow hardware match
    // every view waiting for it, makes the queue wait for its vectors and
    // reopens the command list for the rest of the frame.
    bool RunHardwareFlow(std::vector<ViewRecording>& recordings, FrameSlot& slot, std::string& error) {
#ifdef DEPTHXR_NVOF
        std::vector<D3D12_RESOURCE_BARRIER> barriers;
        for (const ViewRecording& recording : recordings) {
            if (recording.waits_for_hardware && !recording.view->raw_in_use) {
                Require(barriers, recording.view->hardware_newest, D3D12_RESOURCE_STATE_COMMON);
                Require(barriers, recording.view->hardware_previous, D3D12_RESOURCE_STATE_COMMON);
                Require(barriers, recording.view->hardware_flow, D3D12_RESOURCE_STATE_COMMON);
            }
        }
        Flush(barriers);
        Stamp(kProfileHardware);
        HRESULT hr = commands_->Close();
        if (FAILED(hr)) {
            error = Failure("D3D12 Frame Generation command close", hr);
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
        NV_OF_FENCE_POINT ready{fence_.Get(), fence_value_};
        for (std::size_t index = 0; index < recordings.size(); ++index) {
            ViewRecording& recording = recordings[index];
            if (!recording.waits_for_hardware) {
                continue;
            }
            ViewResources& view = *recording.view;
            HardwareWatch* watch = index < hardware_watch_.size() ? &hardware_watch_[index] : nullptr;
            UINT64 wait = 0;
            if (view.raw_in_use) {
                // Matched since the game released the image.
                wait = view.raw_value;
                queue_->Wait(view.hardware_fence.Get(), wait);
                continue;
            }
            NV_OF_EXECUTE_INPUT_PARAMS_D3D12 in{};
            in.inputFrame = view.hardware_buffers[0];
            in.referenceFrame = view.hardware_buffers[1];
            // Its own hints from the last pair, unless the history restarted.
            in.disableTemporalHints =
                recording.temporal && !view.hardware_fresh && view.hardware_mode == 1 ? NV_OF_FALSE : NV_OF_TRUE;
            in.numFencePoints = 1;
            in.fencePoint = &ready;
            NV_OF_FENCE_POINT done{view.hardware_fence.Get(), ++view.hardware_fence_value};
            NV_OF_EXECUTE_OUTPUT_PARAMS_D3D12 out{};
            out.outputBuffer = view.hardware_buffers[2];
            out.fencePoint = &done;
            wait = view.hardware_fence_value;
            if (watch) {
                watch->Watch(wait, ready.value, view.hardware_matched ? 250 : 3000);
            }
            if (stall_hardware_for_test_) {
                stall_hardware_for_test_ = false; // its fence is never signalled
            } else if (hardware_api_.nvOFExecuteD3D12(view.hardware, &in, &out) != NV_OF_SUCCESS) {
                // This frame keeps the last vectors; later frames match on
                // the shader cores again.
                HardwareFailed("the optical flow hardware failed: " + HardwareError(view.hardware));
                queue_->Signal(view.hardware_fence.Get(), wait);
            }
            view.hardware_fresh = false;
            view.hardware_matched = true;
            view.hardware_mode = 1;
            queue_->Wait(view.hardware_fence.Get(), wait);
        }
        hr = slot.after_hardware->Reset();
        if (SUCCEEDED(hr)) hr = commands_->Reset(slot.after_hardware.Get(), nullptr);
        if (FAILED(hr)) {
            error = Failure("D3D12 Frame Generation command reset", hr);
            return false;
        }
        ID3D12DescriptorHeap* heaps[] = {heap_.Get()};
        commands_->SetDescriptorHeaps(1, heaps);
        commands_->SetComputeRootSignature(root_signature_.Get());
        Stamp(kProfileHardware);
        return true;
#else
        static_cast<void>(recordings);
        static_cast<void>(slot);
        error = "Frame Generation has no optical flow hardware support";
        return false;
#endif
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
        case FrameGenTextureKind::kHardwareNewest: return &view.hardware_newest;
        case FrameGenTextureKind::kHardwarePrevious: return &view.hardware_previous;
        case FrameGenTextureKind::kHardwareFlow: return view.raw_in_use ? &view.raw_flow : &view.hardware_flow;
        case FrameGenTextureKind::kTrustNewest: return &view.trust[newest];
        case FrameGenTextureKind::kTrustPrevious: return &view.trust[previous];
        case FrameGenTextureKind::kShiftKeys: return &view.shift_keys;
        case FrameGenTextureKind::kShiftMap: return &view.shift_map;
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

    static bool Generates(const FrameGenDispatch& dispatch) {
        return dispatch.pass == FrameGenPass::kSynthesizeUnorm8 || dispatch.pass == FrameGenPass::kSynthesizeFloat16;
    }

    // Sets a view up for this frame and plans it; RecordPlan records it.
    bool BeginView(const FrameGenViewSubmission& submission, const FrameGenTuning& tuning, ViewResources& view,
                   ViewRecording& recording, bool& synthesized, bool& depth_used, std::string& error) {
        const auto source_it = images_.find(submission.source.swapchain);
        if (source_it == images_.end() || submission.source.image_index >= source_it->second.size() ||
            submission.outputs.size() < tuning.generated) {
            error = "Frame Generation image is not tracked";
            return false;
        }
        std::vector<ID3D12Resource*>& outputs = recording.outputs;
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
        effective.trust_stereo = view.trust_stereo;
        view.trust_stereo = false;
        // The hardware matched this frame against the previous one as soon
        // as the game released it: usable when that previous one is the frame
        // the last Execute generated from.
        const bool captured = view.capture.Matches(submission.source);
        effective.hardware_raw = tuning.hardware_flow && effective.synthesize && captured && view.raw_value &&
                                 view.raw_capture.Matches(submission.source) && view.raw_reference &&
                                 view.raw_reference == view.raw_executed;
        view.raw_in_use = effective.hardware_raw;
        view.raw_executed = captured ? view.raw_captured : 0;
        view.raw_value = effective.hardware_raw ? view.raw_value : 0;
        if (effective.depth && !EnsureTrust(view, submission.request.extent, error)) {
            return false;
        }
        depth_used = effective.depth && effective.synthesize;
        recording.submission = &submission;
        recording.view = &view;
        recording.temporal = effective.temporal;
        recording.plan = BuildFrameGenViewPlan(effective, tuning);
        const FrameGenViewPlan& plan = recording.plan;
        // The generated frames (a synthesis dispatch per output, or one still
        // write for all of them) are drawn into the outputs together when
        // the outputs allow it; they come last in the plan.
        std::vector<const FrameGenDispatch*>& frames = recording.frames;
        frames.assign(tuning.generated, nullptr);
        for (const FrameGenDispatch& dispatch : plan.dispatches) {
            for (std::size_t index = 0; index < frames.size(); ++index) {
                if (Generates(dispatch) && (dispatch.copy_mask & (1u << index))) {
                    frames[index] = &dispatch;
                }
            }
        }
        recording.draw = std::find(frames.begin(), frames.end(), nullptr) == frames.end() &&
                         CanDraw(submission, outputs, frames.size());
        if (!recording.draw && !EnsureOutput(view, submission, error)) {
            return false;
        }
        synthesized = plan.synthesize;
        return true;
    }

    // Records a view's plan from where it stopped; with `until_hardware`,
    // stops right before kFlowUnpack, which needs the optical flow hardware's
    // vectors.
    bool RecordPlan(ViewRecording& recording, bool until_hardware, std::string& error) {
        ViewResources& view = *recording.view;
        const FrameGenViewSubmission& submission = *recording.submission;
        const std::vector<ID3D12Resource*>& outputs = recording.outputs;
        std::vector<D3D12_RESOURCE_BARRIER> barriers;
        recording.waits_for_hardware = false;
        while (recording.next < recording.plan.dispatches.size()) {
            if (until_hardware && recording.plan.dispatches[recording.next].pass == FrameGenPass::kFlowUnpack) {
                // The optical flow hardware's vectors are next.
                recording.waits_for_hardware = true;
                return true;
            }
            const FrameGenDispatch& dispatch = recording.plan.dispatches[recording.next++];
            if (recording.draw && Generates(dispatch)) {
                continue;
            }
            if (descriptor_cursor_ + kDescriptorsPerDispatch > descriptor_end_) {
                error = "D3D12 Frame Generation descriptor budget exhausted";
                return false;
            }
            const UINT base = descriptor_cursor_;
            descriptor_cursor_ += kDescriptorsPerDispatch;
            for (UINT i = 0; i < kShaderResources; ++i) {
                Texture* texture = Resolve(view, dispatch.inputs[i]);
                ShaderResource(texture, base, i);
                if (texture) Require(barriers, *texture, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            }
            for (UINT i = 0; i < 2; ++i) {
                Texture* texture = Resolve(view, dispatch.outputs[i]);
                UnorderedAccess(texture, base + kFirstUav + i);
                if (texture) Require(barriers, *texture, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            }
            Flush(barriers);
            commands_->SetPipelineState(pipelines_[static_cast<std::size_t>(dispatch.pass)].Get());
            commands_->SetComputeRoot32BitConstants(0, sizeof(FrameGenConstants) / 4, &dispatch.constants, 0);
            commands_->SetComputeRootDescriptorTable(1, Gpu(base));
            commands_->SetComputeRootDescriptorTable(2, Gpu(base + kFirstUav));
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
        return !recording.draw || RecordDraw(view, submission, outputs, recording.frames, error);
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
    // Optical flow hardware: loaded on first use; failure turns it off for
    // the backend's life.
#ifdef DEPTHXR_NVOF
    NV_OF_D3D12_API_FUNCTION_LIST hardware_api_{};
#endif
    bool hardware_loaded_{false};
    bool hardware_failed_{false};
    std::string hardware_status_;

    // Watchdog (StartHardwareWatchdog): the operations it watches, and
    // whether it had to release one.
    std::thread hardware_watchdog_;
    std::atomic<bool> hardware_watchdog_stop_{false};
    std::array<HardwareWatch, kWatchedViews> hardware_watch_;
    std::atomic<ULONGLONG> hardware_allowed_ms_{250};
    bool hardware_active_{false};  // the last Execute used the hardware: captures match early
    std::atomic<bool> hardware_stuck_{false};
    std::atomic<bool> hardware_hung_{false}; // the watchdog ever released a fence
    bool stall_hardware_for_test_{false};
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
    std::optional<FrameGenGpuWindow> last_window_;
    std::string dump_directory_; // DumpNextExecute
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
