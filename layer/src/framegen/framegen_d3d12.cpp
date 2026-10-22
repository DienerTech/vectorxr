#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <vulkan/vulkan_core.h>
#include <openxr/openxr_platform.h>

#include "depthxr/framegen_gpu.h"
#include "depthxr/framegen_hardware_watch.h"
#include "depthxr/config_path.h"

#ifdef DEPTHXR_NVOF
#include <nvOpticalFlowD3D12.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

// Quad views: the flow merge (FG_PASS_FLOW_MERGE), a compute shader of its
// own beside the FrameGenPass ones (kFrameGenFlowMergeDxbc).
#include "framegen_flow_merge.h"

namespace depthxr {
namespace {
using Microsoft::WRL::ComPtr;

constexpr std::size_t kFramesInFlight = 3;
constexpr UINT kDescriptorsPerFrame = 1024;
// Captures in flight: four frames of quad views' four color images.
constexpr UINT kCaptureSlots = 16;
// GPU timestamps per Execute: its start, the optical flow hardware's wait
// (before and after it), its end.
constexpr UINT kTimingStamps = 4;
// Just-in-time synthesis: submissions in flight (one per refresh) and the
// descriptors each may use (per view: the shading-rate image, the eye-shift
// map's passes and the draw).
constexpr std::size_t kSynthesisSlots = 4;
constexpr UINT kDescriptorsPerSynthesis = 256;
// A dispatch's descriptors: sampled textures t0-t4, then storage textures
// u0-u1. Depth captures run outside a frame's descriptor range: one
// dispatch per capture slot, after every frame's range.
constexpr UINT kShaderResources = 6;
constexpr UINT kFirstUav = kShaderResources;
constexpr UINT kDescriptorsPerDispatch = kShaderResources + 2;
// Engine motion vectors (CaptureMotion): submissions in flight, never waited
// for, each with a block of descriptors per dispatch (the two eyes'
// censuses, then the resample's, one or one per eye) and a readback of each
// eye's census and samples.
constexpr UINT kMotionSlots = 4;
constexpr UINT kMotionBlocks = 4;
constexpr std::size_t kMotionViews = 2;
constexpr UINT64 kMotionViewReadback = 32 * 1024; // per eye: the census, then the samples at kMotionSamplesOffset
constexpr UINT64 kMotionSamplesOffset = 512;      // D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT
// Censuses read back and not yet taken are kept up to this many (EngineStatus),
// and notes (image descriptions, debug-layer messages) up to this many.
constexpr std::size_t kMotionCensusKept = 32;
constexpr std::size_t kMotionNotes = 64;
// Engine motion evidence (stage 3): each view's solve and summed evidence,
// read back per frame slot (the solve at the view's offset, the evidence
// 512 bytes on).
constexpr UINT64 kEngineViewReadback = 2048;
constexpr UINT64 kEngineEvidenceOffset = 512; // D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT
// Their descriptors start after the captures', and synthesis submissions'
// after theirs.
constexpr UINT kMotionDescriptors = kDescriptorsPerFrame * kFramesInFlight + kCaptureSlots * kDescriptorsPerDispatch;
constexpr UINT kSynthesisDescriptors = kMotionDescriptors + kMotionSlots * kMotionBlocks * kDescriptorsPerDispatch;

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
    // Per real frame, in the history state's ring (FrameGenHistoryRing
    // slots; only those are created).
    std::array<Texture, kFrameGenHistoryRingMax> history;
    std::array<Texture, kFrameGenHistoryRingMax> filtered;
    // FrameGenTuning::cover: the flow filtered for extrapolation as well.
    std::array<Texture, kFrameGenHistoryRingMax> filtered_cover;
    std::array<Texture, kFrameGenPyramidLevels + 1> luma_newest;
    std::array<Texture, kFrameGenPyramidLevels + 1> luma_previous;
    std::array<Texture, kFrameGenPyramidLevels + 1> flow;
    Texture output;
    // Linear depth at half resolution, in the ring like the history: the
    // newest frame's, and the previous one's (content it did not see).
    std::array<Texture, kFrameGenHistoryRingMax> depth;
    // Depth trust scores (in the ring) and the eye-shift map, at half
    // resolution; created once depth is first used.
    std::array<Texture, kFrameGenHistoryRingMax> trust;
    // Quad views (SynthesizeComposed): the view's generated frame, drawn
    // here at its own size for the eyes' compose to read.
    Texture compose_target;
    // Just-in-time synthesis: per ring slot, how often a capture wrote it
    // (an analyzed frame whose slots changed is gone), and the work queue's
    // fence value of the last submission reading it (a capture into it waits
    // for that on the GPU).
    std::array<std::uint32_t, kFrameGenHistoryRingMax> slot_generation{};
    std::array<UINT64, kFrameGenHistoryRingMax> slot_read{};
    std::array<UINT64, kFrameGenHistoryRingMax> slot_read_present{}; // ... of the present queue (synthesis)
    // The last submission using any of the view's images, per fence (the
    // work queue's, the captures' while separate, the present queue's): the
    // images of a view frames no longer have are freed once they have
    // passed (TrimViews).
    UINT64 used_work{0};
    UINT64 used_capture{0};
    UINT64 used_present{0};
    // While recording Synthesize: the ring slots the analyzed frame's
    // newest and previous textures are in (-1: the history state's own).
    int bound_newest{-1};
    int bound_previous{-1};
    Texture shift_keys;
    Texture shift_map; // rebuilt for each generated frame
    Texture motion_map; // FrameGenTuning::depth_splat's, rebuilt for each generated frame
    Texture cross_keys; // kDepthCrossSplat's, for an eye given the other eye's depth (FrameGenDepthEyes)
    Texture depth_stale; // kDepthStale's, for a frame moved by the previous frame's motion
    Texture trust_tiles; // the trust vote's counts per tile, then summed around each
    Texture trust_votes;
    Texture depth_scales; // the stereo depth-scale probe per tile, then the vote on it (raw, accepted)
    Texture depth_scale_raw;
    Texture depth_scale_votes;
    // World motion (FrameGenViewRequest::world_motion): the fit's per-group
    // sums and its result.
    Texture world_partials;
    Texture world_params;
    // Variable-rate shading of the synthesis draw: a rate per tile for the
    // extent and field of view it was made for (see EnsureShadingRate).
    Texture shading_rate;
    FrameGenExtent shading_extent{};
    std::array<float, 4> shading_fov{};
    bool shading_coarse{false}; // the field of view reaches the coarse-shading angle
    bool trust_stereo{false}; // this frame's stereo evidence is in trust[newest]
    bool depth_ready{false};  // captured since the last Execute
    bool depth_fresh{false};  // captured for this Execute's frame (for the check)
    std::uint32_t depth_generation{0}; // FrameGenDepthValidator::Generation() the capture decoded with
    FrameGenDepthEncoding depth_encoding; // as the game describes it
    // Research capture (depth.txt): the depth capture the newest linear
    // depth came from, and what it read.
    UINT64 depth_capture{0};
    FrameGenImage depth_source;
    bool depth_crossed{false}; // the newest depth is the other eye's, moved here (RecordDepthCross)
    // Engine motion vectors (CaptureMotion's `capture`, set up from
    // FrameGenTuning::engine_mode kCapture): the game's raw vectors on the
    // half-resolution grid (kEngineCapture), written in turn (engine_slots_
    // of them); the one the next capture writes; the color capture each one
    // pairs with (none while it waits for the color released after it:
    // engine_pair_next, until the frame's Execute); whether one was captured
    // since the last Execute; and the work queue's fence value of the last
    // analysis that read each (a capture into it waits for that on the GPU).
    std::array<Texture, kFrameGenEngineRawSlots> engine_raw;
    std::uint8_t engine_next{0};
    std::array<CaptureKey, kFrameGenEngineRawSlots> engine_pair{};
    bool engine_pair_next{false};
    bool engine_ready{false};
    std::array<UINT64, kFrameGenEngineRawSlots> engine_read{};
    // Stage 3 (set up from FrameGenTuning::engine_mode kObserve): per raw
    // field, the census of the image it was resampled from (copied with it),
    // that image, how its vectors decode, their region's extent and the
    // field's capture count (engine_captures so far); the count of the field
    // the last Execute took (0: none); and the evidence passes' sums, solve
    // and summed evidence.
    std::array<Texture, kFrameGenEngineRawSlots> engine_census;
    std::array<std::uint32_t, kFrameGenEngineRawSlots> engine_image{};
    std::array<FrameGenMotionEncoding, kFrameGenEngineRawSlots> engine_encoding{};
    std::array<FrameGenExtent, kFrameGenEngineRawSlots> engine_vectors{};
    std::array<std::uint64_t, kFrameGenEngineRawSlots> engine_count{};
    std::uint64_t engine_captures{0};
    std::uint64_t engine_used{0};
    Texture engine_partials;
    Texture engine_params;
    Texture engine_evidence;
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
    FrameGenViewRequest request; // as planned (the history state's effective request)
    FrameGenViewPlan plan;
    std::vector<ID3D12Resource*> outputs;
    std::vector<const FrameGenDispatch*> frames; // the dispatch writing each output
    bool draw{false};
    bool temporal{false};
    std::size_t next{0};
    bool waits_for_hardware{false}; // stopped before kFlowUnpack
    // The view's engine motion vectors were captured for this frame (paired
    // with its color capture): what an analysis of the frame reads. With
    // `evidence`, the plan takes their evidence (stage 3).
    bool engine{false};
    bool evidence{false};
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
    bool hardware_timed{false};   // its stamps around the optical flow hardware were written
    double hardware_call_ms{0.0}; // CPU time in the optical flow hardware's execute calls
    bool analysis_pending{false}; // an analysis whose timing TakeTimings has not returned
    UINT64 capture_mark{0};       // the newest capture submitted before it (0: none)
    ComPtr<ID3D12Resource> check_readback; // depth check scores, read once the slot's fence passes
    bool check_pending{false};
    std::uint32_t check_generation{0}; // FrameGenDepthValidator::CheckGeneration() when recorded
    std::size_t check_source{0};       // the view whose depth the check read (FrameGenDepthEyes)
    // Engine motion evidence (stage 3): every view's solve and summed
    // evidence (kEngineViewReadback each), read once the slot's fence passes,
    // with what it was taken for (the sums filled in then).
    ComPtr<ID3D12Resource> engine_readback;
    bool engine_pending{false};
    FrameGenEngineFrameEvidence engine_frame;
    double engine_seconds{0.0}; // when it was recorded, on the validator's clock
};

struct CaptureSlot {
    ComPtr<ID3D12CommandAllocator> allocator;
    UINT64 fence_value{0};
    bool timed{false}; // its GPU timestamps were resolved and not read yet
};

// An engine motion submission (CaptureMotion), read once its capture fence
// value has passed: what each eye's census in its readback is of.
struct MotionSlot {
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12Resource> readback; // kMotionViews x kMotionViewReadback
    UINT64 fence_value{0};
    bool timed{false};
    struct Pending {
        std::uint64_t sequence{0};
        std::size_t view{0};
        std::uint64_t frame{0};
        bool early{false};
        bool samples{false};
        std::uint32_t images{0};
        std::uint32_t censused{0}; // the images read (bits)
        std::uint32_t color_index{0};
        std::uint32_t depth_index{0};
        XrSwapchain swapchain{XR_NULL_HANDLE};
        XrRect2Di rect{};
    };
    std::vector<Pending> pending; // by eye, in the readback's order
};

// A view's engine motion census outputs, made once.
struct MotionView {
    Texture census;  // R32G32B32A32_UINT, 4 texels per image
    Texture samples; // R32G32B32A32_FLOAT, 16x16 samples of 4 texels
};

// Just-in-time synthesis: a real frame Execute analyzed (analyze_only), as
// Synthesize needs it per view.
struct AnalyzedView {
    bool valid{false};
    std::uint8_t newest{0};   // ring slots of its textures
    std::uint8_t previous{0};
    std::uint32_t newest_generation{0}; // ViewResources::slot_generation when analyzed
    std::uint32_t previous_generation{0};
    FrameGenViewRequest request; // as planned (its shift per unit forward in time)
    bool cover{false};           // kFilteredCover was written
};

struct AnalyzedFrame {
    std::uint64_t id{0};
    FrameGenTuning tuning;
    std::vector<AnalyzedView> views;
    UINT64 work_value{0};    // the work queue's fence when its analysis is done
    UINT64 capture_value{0}; // the capture fence when its images were captured
};

// What the optical flow hardware's watchdog watches, per view (up to
// kWatchedViews): the view's fence and its latest operations, each with the
// fence value it signals when done, the fence_ value its input is ready at
// and how long it may take from then. Each operation keeps its own input:
// a later capture queued behind a stalled one never becomes ready itself.
constexpr std::size_t kWatchedViews = 4;
constexpr std::size_t kWatchedOperations = 4;
using HardwareWatch = FrameGenHardwareWatch<ID3D12Fence, kWatchedOperations>;

#ifdef DEPTHXR_NVOF
// One optical flow session for every view (EnsureAtlas): the views'
// packed images side by side in one image, matched by one execute a
// frame. Beside a game keeping the GPU busy, each submission to the
// hardware waits ~3.5 ms for the GPU and every execute after the first
// ~0.5-2 ms more: four quad views' sessions took 5.8 ms, one atlas 4.8
// (notes/ofa-latency-investigation-2026-10-09.md). Watched as view 0.
struct HardwareAtlas {
    NvOFHandle session{};
    NvOFGPUBufferHandle buffers[3]{}; // newest, previous, vectors
    Texture newest;
    Texture previous;
    Texture flow;
    ComPtr<ID3D12Fence> fence;
    UINT64 fence_value{0};
    NV_OF_PERF_LEVEL perf{NV_OF_PERF_LEVEL_UNDEFINED};
    std::vector<FrameGenExtent> inputs;     // the views' packed images it is laid out for
    std::vector<std::array<UINT, 2>> cells; // where each view's are in it
    bool fresh{true};    // no vectors of its own to hint the next match yet
    bool matched{false}; // matched once (see StartHardwareWatchdog)
};
#endif

class D3D12FrameGen final : public FrameGenGpu {
  public:
    D3D12FrameGen(const XrGraphicsBindingD3D12KHR& binding, ID3D12CommandQueue* runtime_queue)
        : device_(binding.device), queue_(binding.queue), work_(binding.queue), present_(binding.queue) {
        if (runtime_queue) {
            // Queue substitution: the runtime synchronizes with this queue.
            present_ = runtime_queue;
            runtime_on_present_ = true;
        }
        ComPtr<IDXGIFactory4> factory;
        if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
            factory->EnumAdapterByLuid(device_->GetAdapterLuid(), IID_PPV_ARGS(&memory_adapter_));
        }
    }
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
                                        kSynthesisDescriptors + kSynthesisSlots * kDescriptorsPerSynthesis,
                                        D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
        if (!check(device_->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&heap_)), "D3D12 Frame Generation descriptor heap")) {
            return false;
        }
        descriptor_size_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        // CPU-side views for hardware clears (ClearUnorderedAccessViewUint
        // takes one of each); without them clears run as a compute pass.
        D3D12_DESCRIPTOR_HEAP_DESC clear_heap{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kClearViews,
                                              D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        if (FAILED(device_->CreateDescriptorHeap(&clear_heap, IID_PPV_ARGS(&clear_heap_)))) {
            clear_heap_.Reset();
        }
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
        for (auto& slot : motion_slots_) {
            if (!check(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&slot.allocator)),
                       "D3D12 Frame Generation motion allocator")) {
                return false;
            }
        }
        for (auto& slot : synthesis_slots_) {
            if (!check(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&slot.allocator)),
                       "D3D12 Frame Generation synthesis allocator")) {
                return false;
            }
        }
        if (!check(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, synthesis_slots_[0].allocator.Get(),
                                              nullptr, IID_PPV_ARGS(&synthesis_commands_)),
                   "D3D12 Frame Generation synthesis list") ||
            !check(synthesis_commands_->Close(), "D3D12 Frame Generation synthesis list close")) {
            return false;
        }
        // Frame Generation's own work (all but the captures, which must follow
        // the game's rendering on its queue) runs on a queue of its own at
        // high priority: it neither waits behind the game's next frame nor
        // holds that frame up, and frames written right before their refresh
        // are not queued behind it. VECTORXR_FRAMEGEN_QUEUE=game keeps
        // everything on the game's queue.
        // The work queue measures motion (and writes frames together, when
        // not just in time); frames written just in time go on the present
        // queue, the one the runtime synchronizes released images with (its
        // own with queue substitution, else the game's). Kept apart, the
        // present queue only ever holds a refresh's frame, so the game's
        // queue waiting for it (FrameGenQueueSubstitution::AfterWait) never
        // waits for a measurement.
        const char* queue_choice = std::getenv("VECTORXR_FRAMEGEN_QUEUE");
        ComPtr<ID3D12Fence> capture_fence;
        if (SUCCEEDED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&capture_fence)))) {
            if (!(queue_choice && std::string(queue_choice) == "game")) {
                D3D12_COMMAND_QUEUE_DESC desc{D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_QUEUE_PRIORITY_HIGH,
                                              D3D12_COMMAND_QUEUE_FLAG_NONE, 0};
                ComPtr<ID3D12CommandQueue> own;
                if (SUCCEEDED(device_->CreateCommandQueue(&desc, IID_PPV_ARGS(&own)))) {
                    own->SetName(L"VectorXR Frame Generation analysis");
                    work_ = own;
                }
            }
            if (work_.Get() != queue_.Get()) {
                capture_fence_ = capture_fence;
                separate_ = true;
            }
        }
        bool present_ready = true;
        if (present_.Get() != work_.Get() && present_.Get() != queue_.Get() &&
            FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&present_fence_)))) {
            present_ready = false; // frames are written together and handed over (HandOverOutputs)
        }
        // Just-in-time synthesis where the runtime synchronizes with the
        // queue frames are written on (queue substitution): released through
        // the game's queue, a frame written right before its refresh would
        // wait behind the game's queued work. VECTORXR_FRAMEGEN_JIT=0 writes
        // generated frames together, right after each real frame; =1 writes
        // them just in time without substitution (research).
        just_in_time_ = runtime_on_present_;
        if (const char* jit = std::getenv("VECTORXR_FRAMEGEN_JIT")) {
            just_in_time_ = std::string(jit) == "1" || (runtime_on_present_ && std::string(jit) != "0");
        }
        just_in_time_ = just_in_time_ && present_ready;
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
        // Tier 2 variable-rate shading (an image of rates per screen tile)
        // for the synthesis draw, on GPUs: a software rasterizer (WARP) gains
        // nothing and slows down. VECTORXR_FRAMEGEN_VRS sets the tangent off
        // the view axis where coarse shading starts; 0 turns it off.
        D3D12_FEATURE_DATA_D3D12_OPTIONS6 options6{};
        DXGI_ADAPTER_DESC1 adapter{};
        const bool software = memory_adapter_ && SUCCEEDED(memory_adapter_->GetDesc1(&adapter)) &&
                              (adapter.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
        if (const char* vrs = std::getenv("VECTORXR_FRAMEGEN_VRS")) {
            coarse_tangent_ = std::atof(vrs);
        }
        // A software rasterizer (WARP, the tests' device) runs a dispatch on
        // a few CPU threads: its fence waits get far longer than a GPU's
        // watchdog-short ones.
        wait_scale_ = software ? 20 : 1;
        // World motion for world content (VECTORXR_FRAMEGEN_WORLD=0: measured
        // flow as before).
        if (const char* world = std::getenv("VECTORXR_FRAMEGEN_WORLD")) {
            world_motion_ = std::string(world) != "0";
        }
        // An eye whose depth the game leaves clear takes the other eye's
        // (FrameGenDepthEyes; VECTORXR_FRAMEGEN_DEPTH_EYES=0 turns it off).
        if (const char* eyes = std::getenv("VECTORXR_FRAMEGEN_DEPTH_EYES")) {
            depth_eyes_ = std::string(eyes) != "0";
        }
        // Research (depth engine mk2): candidate vectors chosen by depth too
        // (FrameGenTuning::depth_match; VECTORXR_FRAMEGEN_DEPTH_MATCH=1).
        if (const char* match = std::getenv("VECTORXR_FRAMEGEN_DEPTH_MATCH")) {
            depth_match_ = std::string(match) == "1";
        }
        // Research (depth engine mk2): the motion map
        // (FrameGenTuning::depth_splat; VECTORXR_FRAMEGEN_DEPTH_SPLAT=1).
        if (const char* splat = std::getenv("VECTORXR_FRAMEGEN_DEPTH_SPLAT")) {
            depth_splat_ = std::string(splat) == "1";
        }
        // Interpolation's occlusion handling (VECTORXR_FRAMEGEN_OCCLUSION=0:
        // off) and, for research, its cross-fade fallback
        // (VECTORXR_FRAMEGEN_CROSSFADE=1).
        if (const char* occlusion = std::getenv("VECTORXR_FRAMEGEN_OCCLUSION")) {
            occlusion_ = std::string(occlusion) != "0";
        }
        if (const char* fade = std::getenv("VECTORXR_FRAMEGEN_CROSSFADE")) {
            cross_fade_ = std::string(fade) == "1";
        }
        // Engine motion vectors (research): the state a game's motion vector
        // images are taken to be in when they are read at its xrEndFrame
        // (VECTORXR_FRAMEGEN_ENGINE_MV_STATE=auto|rt|common|none; it may never
        // acquire or release them), and the debug layer's messages about
        // them where the game runs with it (VECTORXR_FRAMEGEN_ENGINE_MV_DEBUG=1).
        // Both may come from the research file too (ReadResearchSwitch).
        if (const std::optional<std::string> state = ReadResearchSwitch("VECTORXR_FRAMEGEN_ENGINE_MV_STATE")) {
            const std::string& text = *state;
            motion_state_ = text == "rt"       ? MotionState::kRenderTarget
                            : text == "common" ? MotionState::kCommon
                            : text == "none"   ? MotionState::kNone
                                               : MotionState::kAuto;
        }
        if (ReadResearchSwitch("VECTORXR_FRAMEGEN_ENGINE_MV_DEBUG") == "1") {
            // Only the plain debug layer gives the device this interface.
            if (FAILED(device_.As(&info_queue_))) {
                info_queue_.Reset();
            }
        }
        // Stage 2 (research): the raw fields per view (two by default; three
        // from Observe, whose evidence reads the field before as well, so a
        // capture waits only for an analysis two frames old;
        // VECTORXR_FRAMEGEN_ENGINE_RAW_SLOTS=2|3 sets it), and the fields as
        // RGBA16F, keeping the vectors' b and a
        // (VECTORXR_FRAMEGEN_ENGINE_MV_RESEARCH=z).
        if (const std::optional<std::string> slots = ReadResearchSwitch("VECTORXR_FRAMEGEN_ENGINE_RAW_SLOTS");
            slots == "2" || slots == "3") {
            engine_slots_switch_ = static_cast<std::uint8_t>((*slots)[0] - '0');
            motion_notes_.push_back(std::string(*slots == "3" ? "three" : "two") +
                                    " raw fields per view (VECTORXR_FRAMEGEN_ENGINE_RAW_SLOTS=" + *slots + ")");
        }
        if (ReadResearchSwitch("VECTORXR_FRAMEGEN_ENGINE_MV_RESEARCH") == "z") {
            engine_format_ = DXGI_FORMAT_R16G16B16A16_FLOAT;
            motion_notes_.push_back("raw fields keep the vectors' b and a (VECTORXR_FRAMEGEN_ENGINE_MV_RESEARCH=z)");
        }
        // Stage 3 (research): the vectors' convention fixed
        // (VECTORXR_FRAMEGEN_ENGINE_MV_CONVENTION=gx,gy,camera[,jitter]): the
        // validator only locks that one, or none.
        if (const std::optional<std::string> fixed = ReadResearchSwitch("VECTORXR_FRAMEGEN_ENGINE_MV_CONVENTION")) {
            if (const std::optional<FrameGenEngineConvention> convention = ParseFrameGenEngineConvention(*fixed)) {
                engine_validator_.Fix(*convention);
                motion_notes_.push_back("the vectors' convention is fixed (VECTORXR_FRAMEGEN_ENGINE_MV_CONVENTION=" +
                                        *fixed + "): only it can lock");
            } else {
                motion_notes_.push_back("VECTORXR_FRAMEGEN_ENGINE_MV_CONVENTION=" + *fixed +
                                        " is no convention (gx,gy,included|rotation|excluded|objects[,jitter]): ignored");
            }
        }
        if (!software && coarse_tangent_ > 0.0 &&
            SUCCEEDED(device_->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS6, &options6, sizeof(options6))) &&
            options6.VariableShadingRateTier >= D3D12_VARIABLE_SHADING_RATE_TIER_2 &&
            options6.ShadingRateImageTileSize > 0 && SUCCEEDED(commands_.As(&commands5_)) &&
            SUCCEEDED(synthesis_commands_.As(&synthesis_commands5_))) {
            shading_tile_ = options6.ShadingRateImageTileSize;
        }
        event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        // GPU-aware pacing waits for captures on threads of their own: an
        // event of its own, so no other wait can wake them or be woken.
        capture_wait_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event_ || !capture_wait_event_) {
            error = "D3D12 Frame Generation fence event failed";
            return false;
        }
        D3D12_QUERY_HEAP_DESC queries{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, static_cast<UINT>(kFramesInFlight * kTimingStamps), 0};
        D3D12_HEAP_PROPERTIES readback{D3D12_HEAP_TYPE_READBACK};
        D3D12_RESOURCE_DESC buffer{};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = kFramesInFlight * kTimingStamps * sizeof(UINT64);
        buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
        buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(work_->GetTimestampFrequency(&timestamp_frequency_)) ||
            FAILED(device_->CreateQueryHeap(&queries, IID_PPV_ARGS(&query_heap_))) ||
            FAILED(device_->CreateCommittedResource(&readback, D3D12_HEAP_FLAG_NONE, &buffer,
                                                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback_)))) {
            query_heap_.Reset();
            readback_.Reset();
        }
        // Captures run on the game's queue, timed there: when each finished
        // (engine motion submissions' stamps after the captures').
        D3D12_QUERY_HEAP_DESC capture_queries{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, (kCaptureSlots + kMotionSlots) * 2, 0};
        buffer.Width = (kCaptureSlots + kMotionSlots) * 2 * sizeof(UINT64);
        if (!query_heap_ || FAILED(queue_->GetTimestampFrequency(&capture_frequency_)) ||
            FAILED(device_->CreateQueryHeap(&capture_queries, IID_PPV_ARGS(&capture_query_heap_))) ||
            FAILED(device_->CreateCommittedResource(&readback, D3D12_HEAP_FLAG_NONE, &buffer,
                                                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                    IID_PPV_ARGS(&capture_readback_)))) {
            capture_query_heap_.Reset();
            capture_readback_.Reset();
        }
        // Quad views: composed where frames are written just in time with
        // draws, after the views' flow is merged.
        D3D12_COMPUTE_PIPELINE_STATE_DESC merge{};
        merge.pRootSignature = root_signature_.Get();
        merge.CS = {kFrameGenFlowMergeDxbc, sizeof(kFrameGenFlowMergeDxbc)};
        if (FAILED(device_->CreateComputePipelineState(&merge, IID_PPV_ARGS(&flow_merge_pipeline_)))) {
            flow_merge_pipeline_.Reset();
        }
        supports_composition_ = just_in_time_ && rtv_heap_ && flow_merge_pipeline_ &&
                                ComposePipeline(DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) != nullptr;
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
        HRESULT hr = DeviceRemovedReason();
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
        if (!view.depth[1].resource || !view.history_state.MatchesImages(request.extent, resource->GetDesc().Format,
                                                                      request.output)) {
            error.clear();
            return false;
        }
        const std::size_t slot_index = static_cast<std::size_t>(capture_count_ % capture_slots_.size());
        CaptureSlot& slot = capture_slots_[slot_index];
        if (!WaitFence(slot.fence_value, 100, CaptureFence())) {
            error = "D3D12 Frame Generation capture work did not finish in time";
            return false;
        }
        KeepCaptureEnd(slot, slot_index);
        hr = slot.allocator->Reset();
        if (SUCCEEDED(hr)) hr = capture_commands_->Reset(slot.allocator.Get(), nullptr);
        if (FAILED(hr)) {
            error = Failure("D3D12 Frame Generation capture reset", hr);
            return false;
        }
        StampCapture(slot_index, false);
        // The ring slot written now holds an older frame no longer: frames
        // analyzed from it cannot be synthesized from any more.
        ++view.slot_generation[view.history_state.Newest()];
        std::vector<D3D12_RESOURCE_BARRIER> barriers;
        CopySource(capture_commands_.Get(), barriers, view, source, resource);
        const bool raw = RecordRawPack(view, request, slot_index);
        HardwareWatch* watch = index < hardware_watch_.size() ? &hardware_watch_[index] : nullptr;
        Require(barriers, view.history[view.history_state.Newest()], D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
        if (!barriers.empty()) {
            capture_commands_->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
            barriers.clear();
        }
        StampCapture(slot_index, true);
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
        hr = SubmitCapture(capture_commands_.Get(), view, slot.fence_value);
        if (FAILED(hr)) {
            error = Failure("D3D12 Frame Generation capture signal", hr);
            signal_failed_ = true;
            return false;
        }
        slot.timed = capture_query_heap_ != nullptr;
        ++capture_count_;
        view.capture = {true, source.swapchain, source.image_index, source.array_index, source.rect};
        // Engine motion vectors the game released before this color image
        // (CaptureMotion's `pair_next`) are this frame's: they pair with it.
        if (view.engine_pair_next) {
            view.engine_pair[EngineNewest(view)] = view.capture;
            view.engine_pair_next = false;
            view.engine_ready = true;
        }
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
        // On a work queue of its own the hardware matches the packed images
        // (head movement taken out first) after the capture instead.
        if (separate_ || !hardware_active_ || hardware_failed_ || !view.hardware || !view.raw_flow.resource) {
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
        if (stalled_ || !WaitFence(fence_value_, 2000) ||
            (separate_ && !WaitFence(capture_value_, 2000, CaptureFence())) ||
            (present_fence_ && !WaitFence(present_value_, 2000, present_fence_.Get()))) {
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
#ifdef DEPTHXR_NVOF
        auto atlas_done = [&](const HardwareAtlas& atlas) {
            while (atlas.fence && atlas.fence->GetCompletedValue() < atlas.fence_value) {
                if (GetTickCount64() >= deadline) {
                    return false;
                }
                Sleep(1);
            }
            return true;
        };
        if (!atlas_done(atlas_)) {
            return false;
        }
        for (const HardwareAtlas& atlas : atlas_cache_) {
            if (!atlas_done(atlas)) {
                return false;
            }
        }
#endif
        // Engine motion censuses that ran are read now (EngineStatus), and
        // the evidence, oldest first.
        ReadMotion();
        std::array<FrameSlot*, kFramesInFlight> pending{};
        for (std::size_t i = 0; i < slots_.size(); ++i) pending[i] = &slots_[i];
        std::sort(pending.begin(), pending.end(),
                  [](const FrameSlot* a, const FrameSlot* b) { return a->fence_value < b->fence_value; });
        for (FrameSlot* slot : pending) ReadEngineEvidence(*slot);
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
        if (!view.depth[1].resource || extent.width != request.extent.width || extent.height != request.extent.height) {
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
        HRESULT hr = DeviceRemovedReason();
        if (FAILED(hr)) {
            error = Failure("D3D12 device", hr);
            return false;
        }
        const std::size_t slot_index = capture_count_ % capture_slots_.size();
        CaptureSlot& slot = capture_slots_[slot_index];
        if (!WaitFence(slot.fence_value, 100, CaptureFence())) {
            error = "D3D12 Frame Generation capture work did not finish in time";
            return false;
        }
        KeepCaptureEnd(slot, slot_index);
        hr = slot.allocator->Reset();
        if (SUCCEEDED(hr)) hr = capture_commands_->Reset(slot.allocator.Get(), nullptr);
        if (FAILED(hr)) {
            error = Failure("D3D12 Frame Generation depth capture reset", hr);
            return false;
        }
        StampCapture(slot_index, false);
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
        Texture& linear = view.depth[view.history_state.Newest()];
        ++view.slot_generation[view.history_state.Newest()];
        UnorderedAccess(&linear, base + kFirstUav);
        UnorderedAccess(nullptr, base + kFirstUav + 1);

        // Research capture: the game's own depth texels, as stored.
        RawDepth raw;
        const bool dump_raw = raw_depth_left_ > 0 && PrepareRawDepth(desc, depth.array_index, raw);
        const D3D12_RESOURCE_STATES read =
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
            (dump_raw ? D3D12_RESOURCE_STATE_COPY_SOURCE : D3D12_RESOURCE_STATE_COMMON);
        std::vector<D3D12_RESOURCE_BARRIER> barriers;
        auto flush = [&] {
            if (!barriers.empty()) {
                capture_commands_->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
                barriers.clear();
            }
        };
        Transition(barriers, resource, D3D12_RESOURCE_STATE_DEPTH_WRITE, read);
        Require(barriers, linear, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        flush();
        view.depth_encoding = encoding;
        view.depth_generation = validator_.Generation();
        view.depth_capture = capture_count_;
        view.depth_source = depth;
        view.depth_crossed = false;
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
        if (dump_raw) {
            D3D12_TEXTURE_COPY_LOCATION destination{raw.buffer.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
            destination.PlacedFootprint = raw.footprint;
            D3D12_TEXTURE_COPY_LOCATION source{resource, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
            source.SubresourceIndex = raw.subresource;
            capture_commands_->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        }
        Transition(barriers, resource, read, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        flush();
        StampCapture(slot_index, true);
        hr = capture_commands_->Close();
        if (FAILED(hr)) {
            error = Failure("D3D12 Frame Generation depth capture close", hr);
            view.history_state = {};
            return false;
        }
        hr = SubmitCapture(capture_commands_.Get(), view, slot.fence_value);
        if (FAILED(hr)) {
            error = Failure("D3D12 Frame Generation depth capture signal", hr);
            signal_failed_ = true;
            if (dump_raw) raw.buffer.Detach(); // the GPU may still write it
            return false;
        }
        slot.timed = capture_query_heap_ != nullptr;
        if (dump_raw) {
            --raw_depth_left_;
            WriteRawDepth(raw, index, depth, encoding, slot.fence_value);
        }
        ++capture_count_;
        view.depth_ready = true;
        view.depth_fresh = true;
        return true;
    }

    FrameGenDepthStatus DepthStatus() const override {
        FrameGenDepthStatus status{validator_.Reading(), validator_.Decisions(), validator_.Evidence(), validator_.Scale()};
        status.batches = validator_.Batches();
        status.swapped = validator_.Swapped();
        status.probe_step = validator_.ProbeStep();
        status.empty_view = eyes_.Empty() ? static_cast<int>(*eyes_.Empty()) : -1;
        return status;
    }

    bool SupportsEngineMotion() const override { return true; }

    // Engine motion vectors: each eye's motion vector images fingerprinted
    // (kEngineCensus) on the game's queue, where the game's own rendering
    // ordered before this call has written them, and sampled (stage 0); for
    // jobs with `capture` (stage 2) the image the frame's vectors are in is
    // resampled into the view's raw field as well (kEngineCapture), both eyes
    // in one dispatch when their grids are one size. A game that never
    // acquires or releases them (MSFS 2024) leaves their state unknown:
    // motion_state_ decides the barriers. Census results are read without
    // waiting, at a later call, for EngineStatus.
    bool CaptureMotion(const std::vector<FrameGenMotionJob>& jobs, const std::vector<FrameGenViewRequest>& color,
                       std::string& error, std::vector<bool>* resampled_jobs) override {
        error.clear();
        if (resampled_jobs) resampled_jobs->assign(jobs.size(), false);
        ReadMotion();
        // A research capture's copies of the images (DumpMotion) go with the
        // next census at a frame's end, taken or not: never a later frame's.
        std::string dump_directory;
        bool dump_all = false;
        if (jobs.empty() || !jobs.front().early) {
            dump_directory = std::exchange(motion_dump_directory_, {});
            dump_all = std::exchange(motion_dump_all_, false);
        }
        if (jobs.empty() || motion_stopped_ || stalled_ || signal_failed_) {
            return false;
        }
        HRESULT hr = DeviceRemovedReason();
        if (FAILED(hr)) {
            // Nothing is read for the rest of the session.
            motion_stopped_ = true;
            error = Failure("D3D12 device", hr);
            return false;
        }
        // Each eye's images, refusing what a compute pass cannot read.
        struct Images {
            std::array<ID3D12Resource*, kFrameGenEngineImages> resources{};
            std::uint32_t count{0};
            DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN};
            UINT array_size{1};
            bool simultaneous{false};
        };
        const std::size_t count = std::min(jobs.size(), kMotionViews);
        std::array<Images, kMotionViews> found{};
        for (std::size_t j = 0; j < count; ++j) {
            const FrameGenMotionJob& job = jobs[j];
            const auto it = images_.find(job.vectors.swapchain);
            if (it == images_.end() || it->second.empty()) {
                ++motion_failed_;
                error = "Frame Generation motion vector images are not tracked";
                return false;
            }
            Images& images = found[j];
            const std::size_t available = std::min<std::size_t>(it->second.size(), kFrameGenEngineImages);
            images.count = static_cast<std::uint32_t>(job.image_count ? std::min<std::size_t>(job.image_count, available)
                                                                      : available);
            for (std::uint32_t k = 0; k < images.count; ++k) {
                ID3D12Resource* resource = it->second[k];
                const D3D12_RESOURCE_DESC desc = resource->GetDesc();
                DescribeMotionImage(resource, desc, job.vectors.swapchain, k);
                const DXGI_FORMAT format = MotionViewFormat(desc.Format, job.vectors.format);
                std::string refused;
                if ((desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) != 0) {
                    refused = "they deny shader access";
                } else if (desc.SampleDesc.Count != 1) {
                    refused = "they are multisampled";
                } else if (format == DXGI_FORMAT_UNKNOWN) {
                    refused = "format " + std::to_string(static_cast<int>(desc.Format)) + " holds no float vectors";
                } else if (job.vectors.array_index >= desc.DepthOrArraySize) {
                    refused = "slice " + std::to_string(job.vectors.array_index) + " is not in them";
                }
                if (!refused.empty()) {
                    ++motion_refused_;
                    error = "Frame Generation cannot read the game's motion vectors: " + refused;
                    return false;
                }
                images.resources[k] = resource;
                images.format = format;
                images.array_size = desc.DepthOrArraySize;
                images.simultaneous = (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS) != 0;
            }
        }
        // A free slot, or the call is skipped: never waited for.
        const std::size_t slot_index = static_cast<std::size_t>(motion_count_ % kMotionSlots);
        MotionSlot& slot = motion_slots_[slot_index];
        ID3D12Fence* fence = CaptureFence();
        if (!fence || (slot.fence_value && fence->GetCompletedValue() < slot.fence_value)) {
            ++motion_busy_;
            return false;
        }
        // The views whose vectors are resampled: set up by Execute for engine
        // motion (FrameGenTuning::engine_mode), on their color view's grid,
        // with a color capture to pair with (or one to come). Others are left
        // out this call, counted, their census taken all the same. Vectors
        // the validator disabled are resampled no more (stage 3).
        const bool disabled = engine_validator_.GetState() == FrameGenEngineValidator::State::kDisabled;
        std::array<bool, kMotionViews> resample{};
        for (std::size_t j = 0; j < count; ++j) {
            const FrameGenMotionJob& job = jobs[j];
            if (!job.capture || disabled) {
                continue;
            }
            const ViewResources* view = job.view < views_.size() ? &views_[job.view] : nullptr;
            const FrameGenExtent color_extent = job.view < color.size() ? color[job.view].extent : FrameGenExtent{};
            const FrameGenExtent extent = view ? view->history_state.Extent() : FrameGenExtent{};
            resample[j] = view && view->engine_raw[0].resource && extent.width && extent.width == color_extent.width &&
                          extent.height == color_extent.height && job.vectors.image_index < found[j].count &&
                          (job.pair_next || view->capture.valid);
            motion_unready_ += resample[j] ? 0 : 1;
        }
        if (!slot.pending.empty() || slot.timed) {
            ReadMotion(); // finished since the read above: read before it is reused
        }
        if (!slot.readback) {
            D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_READBACK};
            D3D12_RESOURCE_DESC buffer{};
            buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            buffer.Width = kMotionViews * kMotionViewReadback;
            buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
            buffer.SampleDesc.Count = 1;
            buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            hr = device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST,
                                                  nullptr, IID_PPV_ARGS(&slot.readback));
            if (FAILED(hr)) {
                ++motion_failed_;
                error = Failure("D3D12 Frame Generation motion readback", hr);
                return false;
            }
        }
        if (motion_views_.size() < kMotionViews) {
            motion_views_.resize(kMotionViews);
        }
        for (std::size_t j = 0; j < count; ++j) {
            MotionView& outputs = motion_views_[j];
            if ((!outputs.census.resource &&
                 !Create(outputs.census, {4 * kFrameGenEngineImages, 1}, DXGI_FORMAT_R32G32B32A32_UINT,
                         DXGI_FORMAT_R32G32B32A32_UINT, true, error)) ||
                (!outputs.samples.resource &&
                 !Create(outputs.samples, {16 * 16 * 4, 1}, DXGI_FORMAT_R32G32B32A32_FLOAT,
                         DXGI_FORMAT_R32G32B32A32_FLOAT, true, error))) {
                ++motion_failed_;
                return false;
            }
        }
        // The outputs' states as the GPU will have them before this list,
        // should it never be submitted.
        std::array<std::pair<D3D12_RESOURCE_STATES, D3D12_RESOURCE_STATES>, kMotionViews> output_states{};
        for (std::size_t j = 0; j < count; ++j) {
            output_states[j] = {motion_views_[j].census.state, motion_views_[j].samples.state};
        }
        hr = slot.allocator->Reset();
        if (SUCCEEDED(hr)) hr = capture_commands_->Reset(slot.allocator.Get(), nullptr);
        if (FAILED(hr)) {
            ++motion_failed_;
            error = Failure("D3D12 Frame Generation motion reset", hr);
            return false;
        }
        StampMotion(slot_index, false);
        ID3D12DescriptorHeap* heaps[] = {heap_.Get()};
        capture_commands_->SetDescriptorHeaps(1, heaps);
        capture_commands_->SetComputeRootSignature(root_signature_.Get());
        const bool dumping = !dump_directory.empty();
        std::vector<MotionDump> dumps;
        std::vector<D3D12_RESOURCE_BARRIER> barriers;
        auto flush = [&] {
            if (!barriers.empty()) {
                capture_commands_->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
                barriers.clear();
            }
        };
        // Each eye's census reads every image, or the one the frame's vectors
        // are thought to be in (the image its resample and dump read too).
        std::array<std::uint32_t, kMotionViews> censused{};
        for (std::size_t j = 0; j < count; ++j) {
            const std::uint32_t every = (1u << found[j].count) - 1u;
            const std::uint32_t image = jobs[j].vectors.image_index;
            censused[j] = jobs[j].census_all || image >= found[j].count ? every : 1u << image;
        }
        // Each image read, once (both eyes may read one swapchain's images):
        // from the state it is held in to readable (copyable too when dumped)
        // for the whole list, and back at its end. The images not read keep
        // their state untouched: a game following the specification may hold
        // one it acquired for its next frame in a state of its own.
        const D3D12_RESOURCE_STATES read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                                           (dumping ? D3D12_RESOURCE_STATE_COPY_SOURCE : D3D12_RESOURCE_STATE_COMMON);
        std::vector<std::pair<ID3D12Resource*, D3D12_RESOURCE_STATES>> held_images;
        for (std::size_t j = 0; j < count; ++j) {
            const std::optional<D3D12_RESOURCE_STATES> held = MotionHeldState(found[j].simultaneous);
            // The samples (stage 0) and a dump of every image read them all.
            const std::uint32_t every = (1u << found[j].count) - 1u;
            const std::uint32_t touched = censused[j] | (jobs[j].samples || (dumping && dump_all) ? every : 0u);
            for (std::uint32_t k = 0; k < found[j].count && held; ++k) {
                if (((touched >> k) & 1u) == 0) {
                    continue;
                }
                ID3D12Resource* resource = found[j].resources[k];
                if (std::none_of(held_images.begin(), held_images.end(),
                                 [&](const auto& image) { return image.first == resource; })) {
                    held_images.emplace_back(resource, *held);
                    Transition(barriers, resource, *held, read);
                }
            }
        }
        // The censuses: a dispatch per eye, in the slot's first blocks.
        capture_commands_->SetPipelineState(pipelines_[static_cast<std::size_t>(FrameGenPass::kEngineCensus)].Get());
        std::vector<MotionSlot::Pending> pending;
        std::vector<std::size_t> depth_views;
        for (std::size_t j = 0; j < count; ++j) {
            const FrameGenMotionJob& job = jobs[j];
            const Images& images = found[j];
            MotionView& outputs = motion_views_[j];
            const UINT base = MotionBlock(slot_index, j);
            for (std::uint32_t k = 0; k < kFrameGenEngineImages; ++k) {
                MotionImageView(k < images.count ? images.resources[k] : nullptr, images.format, images.array_size,
                                base + k);
            }
            // This frame's linear depth (captured at its release, since the
            // last Execute), where the samples are taken.
            const FrameGenExtent color_extent = job.view < color.size() ? color[job.view].extent : FrameGenExtent{};
            const FrameGenExtent half = FrameGenPyramid(color_extent)[1];
            Texture* depth = nullptr;
            if (job.samples && job.view < views_.size()) {
                ViewResources& view = views_[job.view];
                Texture& newest = view.depth[view.history_state.Newest()];
                const FrameGenExtent extent = view.history_state.Extent();
                if (view.depth_ready && newest.resource && extent.width == color_extent.width &&
                    extent.height == color_extent.height) {
                    depth = &newest;
                    Require(barriers, newest, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                    depth_views.push_back(job.view);
                }
            }
            ShaderResource(depth, base, 3);
            for (UINT i = 4; i < kShaderResources; ++i) {
                ShaderResource(nullptr, base, i);
            }
            const bool samples = job.samples && half.width > 0 && half.height > 0;
            UnorderedAccess(&outputs.census, base + kFirstUav);
            UnorderedAccess(samples ? &outputs.samples : nullptr, base + kFirstUav + 1);
            Require(barriers, outputs.census, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            if (samples) Require(barriers, outputs.samples, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            flush();
            FrameGenConstants constants{};
            constants.fov_current = {static_cast<float>(job.vectors.rect.offset.x), static_cast<float>(job.vectors.rect.offset.y),
                                     static_cast<float>(job.vectors.rect.extent.width),
                                     static_cast<float>(job.vectors.rect.extent.height)};
            constants.size = {half.width, half.height, images.count, job.vectors.array_index};
            constants.options = {(samples ? 1u : 0u) | (depth ? 2u : 0u) | (censused[j] << 8), static_cast<std::uint32_t>(job.frame),
                                 job.color_index, job.depth_index};
            capture_commands_->SetComputeRoot32BitConstants(0, sizeof(FrameGenConstants) / 4, &constants, 0);
            capture_commands_->SetComputeRootDescriptorTable(1, Gpu(base));
            capture_commands_->SetComputeRootDescriptorTable(2, Gpu(base + kFirstUav));
            capture_commands_->Dispatch(images.count + (samples ? 1u : 0u), 1, 1);
            if (dumping) {
                for (std::uint32_t k = 0; k < images.count; ++k) {
                    if (dump_all || k == job.vectors.image_index) {
                        DumpMotionImage(images.resources[k], job, k, dump_all, dumps);
                    }
                }
            }
            MotionSlot::Pending entry;
            entry.sequence = ++motion_sequence_;
            entry.view = job.view;
            entry.frame = job.frame;
            entry.early = job.early;
            entry.samples = samples;
            entry.images = images.count;
            entry.censused = censused[j];
            entry.color_index = job.color_index;
            entry.depth_index = job.depth_index;
            entry.swapchain = job.vectors.swapchain;
            entry.rect = job.vectors.rect;
            pending.push_back(entry);
        }
        // The resample (kEngineCapture) into each view's next raw field, in
        // the slot's last blocks: one dispatch for eyes of one grid, else one
        // each.
        std::vector<std::size_t> resampled;
        for (std::size_t j = 0; j < count; ++j) {
            if (resample[j]) resampled.push_back(j);
        }
        std::vector<std::vector<std::size_t>> dispatches;
        for (const std::size_t j : resampled) {
            const FrameGenExtent l1 = FrameGenPyramid(color[jobs[j].view].extent)[1];
            const auto same = std::find_if(dispatches.begin(), dispatches.end(), [&](const std::vector<std::size_t>& eyes) {
                const FrameGenExtent other = FrameGenPyramid(color[jobs[eyes.front()].view].extent)[1];
                return other.width == l1.width && other.height == l1.height;
            });
            if (same != dispatches.end()) {
                same->push_back(j);
            } else {
                dispatches.push_back({j});
            }
        }
        if (!dispatches.empty()) {
            capture_commands_->SetPipelineState(pipelines_[static_cast<std::size_t>(FrameGenPass::kEngineCapture)].Get());
        }
        for (std::size_t d = 0; d < dispatches.size(); ++d) {
            const std::vector<std::size_t>& eyes = dispatches[d];
            const UINT base = MotionBlock(slot_index, kMotionViews + d);
            std::array<std::array<std::int32_t, 4>, 2> rects{};
            std::array<std::uint32_t, 2> slices{};
            for (std::size_t e = 0; e < 2; ++e) {
                // A single eye reads its own image in both places.
                const std::size_t j = eyes[std::min(e, eyes.size() - 1)];
                const FrameGenMotionJob& job = jobs[j];
                MotionImageView(found[j].resources[job.vectors.image_index], found[j].format, found[j].array_size,
                                base + static_cast<UINT>(e));
                const XrRect2Di& rect = job.vectors.rect;
                rects[e] = {rect.offset.x, rect.offset.y, rect.extent.width, rect.extent.height};
                slices[e] = job.vectors.array_index;
                ViewResources& view = views_[job.view];
                Texture* raw = e < eyes.size() ? &view.engine_raw[view.engine_next] : nullptr;
                UnorderedAccess(raw, base + kFirstUav + static_cast<UINT>(e));
                if (raw) Require(barriers, *raw, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            }
            for (UINT i = 2; i < kShaderResources; ++i) {
                ShaderResource(nullptr, base, i);
            }
            flush();
            const FrameGenDispatch dispatch = BuildFrameGenMotionCaptureDispatch(
                FrameGenPyramid(color[jobs[eyes.front()].view].extent)[1], rects[0], rects[1], slices[0], slices[1],
                static_cast<std::uint32_t>(eyes.size()));
            capture_commands_->SetComputeRoot32BitConstants(0, sizeof(FrameGenConstants) / 4, &dispatch.constants, 0);
            capture_commands_->SetComputeRootDescriptorTable(1, Gpu(base));
            capture_commands_->SetComputeRootDescriptorTable(2, Gpu(base + kFirstUav));
            capture_commands_->Dispatch((dispatch.width + kFrameGenThreadGroupSize - 1) / kFrameGenThreadGroupSize,
                                        (dispatch.height + kFrameGenThreadGroupSize - 1) / kFrameGenThreadGroupSize,
                                        static_cast<UINT>(eyes.size()));
        }
        for (const auto& [resource, held] : held_images) {
            Transition(barriers, resource, read, held);
        }
        // The censuses into the slot's readback, each eye's part of it.
        for (std::size_t j = 0; j < count; ++j) {
            Require(barriers, motion_views_[j].census, D3D12_RESOURCE_STATE_COPY_SOURCE);
            if (pending[j].samples) Require(barriers, motion_views_[j].samples, D3D12_RESOURCE_STATE_COPY_SOURCE);
        }
        flush();
        for (std::size_t j = 0; j < count; ++j) {
            CopyToReadback(capture_commands_.Get(), motion_views_[j].census, slot.readback.Get(), j * kMotionViewReadback);
            if (pending[j].samples) {
                CopyToReadback(capture_commands_.Get(), motion_views_[j].samples, slot.readback.Get(),
                               j * kMotionViewReadback + kMotionSamplesOffset);
            }
        }
        // Stage 3: each field resampled keeps its image's census beside it
        // (the evidence's stale test reads it against the field before's).
        for (const std::size_t j : resampled) {
            ViewResources& view = views_[jobs[j].view];
            Texture& census = view.engine_census[view.engine_next];
            if (census.resource) {
                Require(barriers, census, D3D12_RESOURCE_STATE_COPY_DEST);
                flush();
                capture_commands_->CopyResource(census.resource.Get(), motion_views_[j].census.resource.Get());
            }
        }
        StampMotion(slot_index, true);
        hr = capture_commands_->Close();
        if (FAILED(hr)) {
            // Nothing was submitted: the outputs' tracked states go back to
            // what the GPU has. Earlier submissions may still use them, so
            // they are never freed here; the depth views and those resampled
            // are rebuilt as Capture does (waiting for the GPU before freeing).
            ++motion_failed_;
            error = Failure("D3D12 Frame Generation motion close", hr);
            for (std::size_t j = 0; j < count; ++j) {
                motion_views_[j].census.state = output_states[j].first;
                motion_views_[j].samples.state = output_states[j].second;
            }
            for (const std::size_t v : depth_views) views_[v].history_state = {};
            for (const std::size_t j : resampled) views_[jobs[j].view].history_state = {};
            return false;
        }
        std::vector<ViewResources*> written;
        for (const std::size_t j : resampled) {
            written.push_back(&views_[jobs[j].view]);
        }
        UINT64 value = 0;
        hr = SubmitMotion(capture_commands_.Get(), written, value);
        for (const std::size_t v : depth_views) {
            MarkUsed(views_[v], CaptureFence(), value);
        }
        for (ViewResources* view : written) {
            MarkUsed(*view, CaptureFence(), value);
        }
        if (FAILED(hr)) {
            // The slot stays busy for good: its list may still run (as a
            // capture slot's, see SubmitCapture).
            slot.fence_value = value;
            ++motion_failed_;
            error = Failure("D3D12 Frame Generation motion signal", hr);
            signal_failed_ = true;
            for (MotionDump& dump : dumps) dump.buffer.Detach(); // the GPU may still write them
            return false;
        }
        slot.fence_value = value;
        slot.timed = capture_query_heap_ != nullptr;
        slot.pending = std::move(pending);
        ++motion_count_;
        ++motion_submitted_;
        motion_submitted_at_ = std::chrono::steady_clock::now();
        // Each raw field written pairs with the view's color capture: the
        // frame's own when it was captured first, else its next (Capture).
        for (const std::size_t j : resampled) {
            ViewResources& view = views_[jobs[j].view];
            const std::uint8_t field = view.engine_next;
            view.engine_pair[field] = jobs[j].pair_next ? CaptureKey{} : view.capture;
            view.engine_pair_next = jobs[j].pair_next;
            view.engine_ready = !jobs[j].pair_next;
            view.engine_image[field] = jobs[j].vectors.image_index;
            view.engine_encoding[field] = jobs[j].encoding;
            view.engine_vectors[field] = {static_cast<std::uint32_t>(std::max(jobs[j].vectors.rect.extent.width, 0)),
                                          static_cast<std::uint32_t>(std::max(jobs[j].vectors.rect.extent.height, 0))};
            view.engine_count[field] = ++view.engine_captures;
            view.engine_next = static_cast<std::uint8_t>((field + 1u) % engine_slots_);
            ++motion_captured_;
            if (resampled_jobs) (*resampled_jobs)[j] = true;
        }
        motion_dispatches_ += dispatches.size();
        if (!dumps.empty()) {
            WriteMotionDump(std::move(dumps), value, dump_directory);
            ++motion_dumped_;
        }
        ReadDebugMessages();
        return true;
    }

    FrameGenEngineStatus EngineStatus(std::uint64_t after = 0) const override {
        FrameGenEngineStatus status;
        status.submitted = motion_submitted_;
        status.busy = motion_busy_;
        status.refused = motion_refused_;
        status.failed = motion_failed_;
        status.dumped = motion_dumped_;
        status.captured = motion_captured_;
        status.dispatches = motion_dispatches_;
        status.waits = motion_waits_;
        status.unready = motion_unready_;
        status.paired = motion_paired_;
        status.evidence = engine_evidence_frames_;
        status.validation = engine_validator_.Report();
        status.stopped = motion_stopped_;
        status.notes = motion_notes_;
        for (const FrameGenEngineCensus& census : motion_census_) {
            if (census.sequence > after) status.census.push_back(census);
        }
        return status;
    }

    void SetEnginePrior(const FrameGenEngineConvention& convention) override { engine_validator_.SetPrior(convention); }

    bool DumpMotion(const std::string& directory, bool all_images) override {
        motion_dump_directory_ = directory;
        motion_dump_all_ = all_images;
        return true;
    }

    // A game that fills only one eye's depth (FrameGenDepthEyes): the other
    // eye's linear depth moved to the empty eye's view (kDepthCrossSplat
    // clears the keys, then splats; kDepthCrossResolve writes the empty eye's
    // newest depth), before the trust pass and the plans read it. Both views'
    // newest images must be this frame's.
    bool RecordDepthCross(const FrameGenViewSubmission& source_submission,
                          const FrameGenViewSubmission& target_submission, ViewResources& source,
                          ViewResources& target, std::string& error) {
        const FrameGenExtent half = FrameGenPyramid(target_submission.request.extent)[1];
        if (!target.cross_keys.resource &&
            !Create(target.cross_keys, half, DXGI_FORMAT_R32_UINT, DXGI_FORMAT_R32_UINT, true, error)) {
            return false;
        }
        Texture& from = source.depth[source.history_state.Newest()];
        Texture& to = target.depth[target.history_state.Newest()];
        std::vector<D3D12_RESOURCE_BARRIER> barriers;
        const auto run = [&](FrameGenPass pass, std::uint32_t mode, Texture* input, Texture& output) {
            if (descriptor_cursor_ + kDescriptorsPerDispatch > descriptor_end_) {
                error = "D3D12 Frame Generation descriptor budget exhausted";
                return false;
            }
            const UINT base = descriptor_cursor_;
            descriptor_cursor_ += kDescriptorsPerDispatch;
            ShaderResource(input, base, 0);
            for (UINT i = 1; i < kShaderResources; ++i) {
                ShaderResource(nullptr, base, i);
            }
            if (input) Require(barriers, *input, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            UnorderedAccess(&output, base + kFirstUav);
            UnorderedAccess(nullptr, base + kFirstUav + 1);
            Require(barriers, output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Flush(barriers);
            const FrameGenDispatch dispatch =
                BuildFrameGenDepthCrossDispatch(pass, mode, source_submission.request.extent,
                                                target_submission.request.extent, source_submission.request.newest,
                                                target_submission.request.newest);
            commands_->SetPipelineState(pipelines_[static_cast<std::size_t>(pass)].Get());
            commands_->SetComputeRoot32BitConstants(0, sizeof(FrameGenConstants) / 4, &dispatch.constants, 0);
            commands_->SetComputeRootDescriptorTable(1, Gpu(base));
            commands_->SetComputeRootDescriptorTable(2, Gpu(base + kFirstUav));
            commands_->Dispatch((dispatch.width + kFrameGenThreadGroupSize - 1) / kFrameGenThreadGroupSize,
                                (dispatch.height + kFrameGenThreadGroupSize - 1) / kFrameGenThreadGroupSize, 1);
            return true;
        };
        if (!run(FrameGenPass::kDepthCrossSplat, 0, &from, target.cross_keys) ||
            !run(FrameGenPass::kDepthCrossSplat, 1, &from, target.cross_keys) ||
            !run(FrameGenPass::kDepthCrossResolve, 0, &target.cross_keys, to)) {
            return false;
        }
        target.depth_ready = true;
        target.depth_generation = source.depth_generation;
        target.depth_encoding = source.depth_encoding;
        target.depth_crossed = true;
        return true;
    }

    // Research capture: a readback buffer for one array slice of the game's
    // depth image (the depth plane), and where in it the copy lands.
    struct RawDepth {
        ComPtr<ID3D12Resource> buffer;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT subresource{0};
        UINT rows{0};
        UINT64 row_bytes{0};
    };

    bool PrepareRawDepth(const D3D12_RESOURCE_DESC& desc, std::uint32_t array_index, RawDepth& raw) {
        raw.subresource = array_index * std::max<UINT>(desc.MipLevels, 1u); // mip 0, depth plane
        UINT64 bytes = 0;
        device_->GetCopyableFootprints(&desc, raw.subresource, 1, 0, &raw.footprint, &raw.rows, &raw.row_bytes, &bytes);
        if (!bytes || bytes == UINT64_MAX) {
            return false;
        }
        D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_READBACK};
        D3D12_RESOURCE_DESC buffer{};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = bytes;
        buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
        buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        return SUCCEEDED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                                          D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                          IID_PPV_ARGS(&raw.buffer)));
    }

    // Writes the texels the capture at `value` copied into `raw`, and what
    // they are, next to the capture's frames: depth-raw-<capture>-view<i>.bin
    // and .txt. Hundreds of megabytes: a thread of its own waits for the GPU
    // and writes them (the release thread never waits), keeping the buffer
    // until it is done, or for good if the GPU never finishes with it.
    void WriteRawDepth(RawDepth& raw, std::size_t index, const FrameGenImage& depth,
                       const FrameGenDepthEncoding& encoding, UINT64 value) {
        const std::string stem = raw_depth_root_ + "\\depth-raw-" + std::to_string(capture_count_) + "-view" +
                                 std::to_string(index);
        std::ostringstream meta;
        meta.precision(9);
        meta << "capture " << capture_count_ << "\nview " << index << "\nswapchain " << depth.swapchain << "\nimage "
             << depth.image_index << "\nslice " << depth.array_index << "\nrect " << depth.rect.offset.x << ' '
             << depth.rect.offset.y << ' ' << depth.rect.extent.width << ' ' << depth.rect.extent.height
             << "\nformat " << static_cast<int>(raw.footprint.Footprint.Format) << "\nwidth "
             << raw.footprint.Footprint.Width << "\nheight " << raw.footprint.Footprint.Height << "\nrow_pitch "
             << raw.footprint.Footprint.RowPitch << "\nrow_bytes " << raw.row_bytes << "\nrows " << raw.rows
             << "\nmin_depth " << encoding.min_depth << "\nmax_depth " << encoding.max_depth << "\nnear_z "
             << encoding.near_z << "\nfar_z " << encoding.far_z << "\nreading "
             << ToString(validator_.Reading()) << "\nswapped " << validator_.Swapped() << "\nscale "
             << validator_.Scale() << '\n';
        std::thread([buffer = std::move(raw.buffer), footprint = raw.footprint, rows = raw.rows,
                     row_bytes = raw.row_bytes, stem, meta = meta.str(), fence = ComPtr<ID3D12Fence>(CaptureFence()),
                     value, timeout = static_cast<DWORD>(10000 * wait_scale_)]() mutable {
            if (!WaitOnDumpThread(fence.Get(), value, timeout)) {
                buffer.Detach(); // the GPU may still write it: never freed under it
                return;
            }
            std::ofstream(stem + ".txt") << meta;
            const std::string path = stem + ".bin";
            void* mapped = nullptr;
            if (FAILED(buffer->Map(0, nullptr, &mapped))) {
                return;
            }
            std::ofstream file(path, std::ios::binary);
            for (UINT row = 0; row < rows && file; ++row) {
                file.write(static_cast<const char*>(mapped) + footprint.Offset + row * footprint.Footprint.RowPitch,
                           static_cast<std::streamsize>(row_bytes));
            }
            buffer->Unmap(0, nullptr);
        }).detach();
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
                                             &other.history[other.history_state.Newest()],
                                             &view.depth[view.history_state.Newest()], nullptr, nullptr};
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

        // The stereo depth-scale probe, from the same images (FG_PASS_DEPTH_SCALE).
        if (descriptor_cursor_ + kDescriptorsPerDispatch > descriptor_end_) {
            error = "D3D12 Frame Generation descriptor budget exhausted";
            return false;
        }
        const UINT scale_base = descriptor_cursor_;
        descriptor_cursor_ += kDescriptorsPerDispatch;
        for (UINT i = 0; i < kShaderResources; ++i) {
            ShaderResource(inputs[i], scale_base, i);
        }
        UnorderedAccess(&view.depth_scales, scale_base + kFirstUav);
        UnorderedAccess(nullptr, scale_base + kFirstUav + 1);
        Require(barriers, view.depth_scales, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Flush(barriers);
        const FrameGenDispatch scale = BuildFrameGenDepthScaleDispatch(submission.request.extent, submission.request.newest,
                                                                       other_submission.request.newest);
        commands_->SetPipelineState(pipelines_[static_cast<std::size_t>(FrameGenPass::kDepthScale)].Get());
        commands_->SetComputeRoot32BitConstants(0, sizeof(FrameGenConstants) / 4, &scale.constants, 0);
        commands_->SetComputeRootDescriptorTable(1, Gpu(scale_base));
        commands_->SetComputeRootDescriptorTable(2, Gpu(scale_base + kFirstUav));
        commands_->Dispatch((scale.width + kFrameGenThreadGroupSize - 1) / kFrameGenThreadGroupSize,
                            (scale.height + kFrameGenThreadGroupSize - 1) / kFrameGenThreadGroupSize, 1);
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
        // Scores in [-1, 1]: half floats hold them to a thousandth.
        for (std::uint8_t slot = 0; slot < view.history_state.Ring(); ++slot) {
            if (!Create(view.trust[slot], half, DXGI_FORMAT_R16_FLOAT, DXGI_FORMAT_R16_FLOAT, true, error)) return false;
        }
        const FrameGenExtent tiles = FrameGenTrustTiles(half);
        return Create(view.depth_scales, {tiles.width * kFrameGenDepthScaleTexels, tiles.height},
                      DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT, true, error) &&
               Create(view.depth_scale_raw, tiles, DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT,
                      true, error) &&
               Create(view.depth_scale_votes, tiles, DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT,
                      true, error) &&
               Create(view.trust_tiles, tiles, DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT, true,
                      error) &&
               Create(view.trust_votes, tiles, DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT, true,
                      error) &&
               Create(view.shift_keys, half, DXGI_FORMAT_R32_UINT, DXGI_FORMAT_R32_UINT, true, error) &&
               Create(view.depth_stale, half, DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT, true, error) &&
               Create(view.shift_map, half, DXGI_FORMAT_R32_UINT, DXGI_FORMAT_R32_UINT, true, error);
    }

    // A view's world-motion fit textures, created the first time it is used.
    bool EnsureWorld(ViewResources& view, std::string& error) {
        if (view.world_params.resource) {
            return true;
        }
        const std::uint32_t groups = (kFrameGenWorldGrid / kFrameGenThreadGroupSize) * (kFrameGenWorldGrid / kFrameGenThreadGroupSize);
        return Create(view.world_partials, {groups * 8u, 1u}, DXGI_FORMAT_R32G32B32A32_FLOAT,
                      DXGI_FORMAT_R32G32B32A32_FLOAT, true, error) &&
               Create(view.world_params, {2u, 1u}, DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT, true,
                      error);
    }

    // A 2D shader resource view of `texture` (a null one without) for
    // register t`slot` of the dispatch whose descriptors start at `base`.
    // The fifth and sixth are read as unsigned integers.
    void ShaderResource(Texture* texture, UINT base, UINT slot) {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = texture ? texture->view_format : slot >= 4 ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_R8G8B8A8_UNORM;
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
        // The other eye's depth (t3) only says whether it varies at all
        // (FrameGenDepthEyes, with depth_eyes_).
        Texture* other_depth = depth_eyes_ ? &view1.depth[view1.history_state.Newest()] : nullptr;
        Texture* inputs[kShaderResources] = {&view0.history[view0.history_state.Newest()],
                                             &view1.history[view1.history_state.Newest()],
                                             &view0.depth[view0.history_state.Newest()],
                                             other_depth && other_depth->resource ? other_depth : nullptr, nullptr};
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
                    evidence.source_varied += texel[2];
                    evidence.other_varied += texel[3];
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
        // An eye whose depth is clear while the other eye's is not
        // (FrameGenDepthEyes): the other eye is checked from now on, and
        // judged afresh.
        if (depth_eyes_ && eyes_.Add(slot.check_source, evidence)) {
            validator_.Restart();
            return;
        }
        // A check of depth decoded before the validator last changed the
        // decoding, or probed with another step, scored other readings.
        if (slot.check_generation == validator_.CheckGeneration()) {
            validator_.Add(evidence);
        }
    }

    // Engine motion vectors (stage 3): copies each view's solve and summed
    // evidence (written by the plan's kEngineSolve) into the slot's readback,
    // for ReadEngineEvidence once the slot's work is done; what it was taken
    // for goes with it. False where the readback could not be made: no
    // evidence from then on this session, and Frame Generation goes on as it
    // was (research never costs it a frame).
    bool RecordEngineReadback(FrameSlot& slot, const std::vector<ViewRecording>& recordings,
                              const FrameGenTuning& tuning) {
        if (!slot.engine_readback) {
            D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_READBACK};
            D3D12_RESOURCE_DESC buffer{};
            buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            buffer.Width = kMotionViews * kEngineViewReadback;
            buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
            buffer.SampleDesc.Count = 1;
            buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            const HRESULT hr = device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
                                                                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                                IID_PPV_ARGS(&slot.engine_readback));
            if (FAILED(hr)) {
                engine_failed_ = true;
                if (motion_notes_.size() < kMotionNotes) {
                    motion_notes_.push_back("the evidence readback could not be made (" +
                                            Failure("D3D12 Frame Generation engine evidence readback", hr) +
                                            "): no evidence this session, Frame Generation goes on without it");
                }
                return false;
            }
        }
        std::vector<D3D12_RESOURCE_BARRIER> barriers;
        FrameGenEngineFrameEvidence& frame = slot.engine_frame;
        frame = {};
        frame.convention = tuning.engine;
        frame.locked = tuning.engine_locked;
        for (std::size_t i = 0; i < recordings.size() && i < kMotionViews; ++i) {
            ViewResources& view = *recordings[i].view;
            Require(barriers, view.engine_params, D3D12_RESOURCE_STATE_COPY_SOURCE);
            Require(barriers, view.engine_evidence, D3D12_RESOURCE_STATE_COPY_SOURCE);
            Flush(barriers);
            CopyToReadback(commands_.Get(), view.engine_params, slot.engine_readback.Get(), i * kEngineViewReadback);
            CopyToReadback(commands_.Get(), view.engine_evidence, slot.engine_readback.Get(),
                           i * kEngineViewReadback + kEngineEvidenceOffset);
            FrameGenEngineEvidence evidence;
            evidence.calibrate = recordings[i].request.engine_calibrate;
            evidence.l1 = recordings[i].plan.levels[1];
            evidence.vectors = recordings[i].request.engine_vectors;
            evidence.color = recordings[i].request.extent;
            frame.views.push_back(evidence);
        }
        slot.engine_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - engine_clock_).count();
        return true;
    }

    // Feeds a finished slot's evidence to the engine validator.
    void ReadEngineEvidence(FrameSlot& slot) {
        if (!slot.engine_pending || !slot.engine_readback || fence_->GetCompletedValue() < slot.fence_value ||
            FAILED(device_->GetDeviceRemovedReason())) {
            return;
        }
        slot.engine_pending = false;
        void* mapped = nullptr;
        const D3D12_RANGE range{0, static_cast<SIZE_T>(kMotionViews * kEngineViewReadback)};
        if (FAILED(slot.engine_readback->Map(0, &range, &mapped)) || !mapped) {
            return;
        }
        FrameGenEngineFrameEvidence& frame = slot.engine_frame;
        for (std::size_t i = 0; i < frame.views.size(); ++i) {
            const auto* base = static_cast<const std::byte*>(mapped) + i * kEngineViewReadback;
            FrameGenEngineEvidence read = ReadFrameGenEngineEvidence(
                reinterpret_cast<const float*>(base + kEngineEvidenceOffset), reinterpret_cast<const float*>(base));
            read.calibrate = frame.views[i].calibrate;
            read.l1 = frame.views[i].l1;
            read.vectors = frame.views[i].vectors;
            read.color = frame.views[i].color;
            frame.views[i] = read;
        }
        const D3D12_RANGE written{0, 0};
        slot.engine_readback->Unmap(0, &written);
        engine_validator_.Add(frame, slot.engine_seconds);
        ++engine_evidence_frames_;
        if (engine_validator_.GetState() == FrameGenEngineValidator::State::kDisabled && !engine_disable_noted_ &&
            motion_notes_.size() < kMotionNotes) {
            engine_disable_noted_ = true;
            motion_notes_.push_back("the vectors are disabled (" + engine_validator_.Report().reason +
                                    "): no more are resampled this session");
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
        const ComPtr<ID3D12PipelineState> pipeline = CreateDrawPipeline(framegen_shaders::kDxbcDraw[count], formats.data(), count);
        draw_pipelines_[key] = pipeline;
        return pipeline.Get();
    }

    // A full-screen triangle (the draw vertex shader) shaded by `pixel` into
    // `count` render targets of these formats, nothing blended; null if it
    // cannot be created.
    ComPtr<ID3D12PipelineState> CreateDrawPipeline(const framegen_shaders::Blob& pixel, const DXGI_FORMAT* formats,
                                                   std::size_t count) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = root_signature_.Get();
        desc.VS = {framegen_shaders::kDxbcDraw[0].data, framegen_shaders::kDxbcDraw[0].size};
        desc.PS = {pixel.data, pixel.size};
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
        return pipeline;
    }

    // Whether every generated frame of the view can be drawn straight into
    // its output image: renderable, single-sampled, a typed format, and the
    // whole image at the view's size; together, or `one_at_a_time`.
    bool CanDraw(const FrameGenViewSubmission& submission, const std::vector<ID3D12Resource*>& outputs,
                 std::size_t count, bool one_at_a_time) {
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
        if (one_at_a_time) {
            // In batches of consecutive frames (RecordPlan), any of them alone.
            for (std::size_t first = 0; first < count; ++first) {
                std::array<DXGI_FORMAT, kFrameGenMaxGenerated> batch{};
                for (std::size_t last = first; last < count; ++last) {
                    batch[last - first] = formats[last];
                    if (!DrawPipeline(batch, last - first + 1)) {
                        return false;
                    }
                }
            }
            return true;
        }
        return DrawPipeline(formats, count) != nullptr;
    }

    bool ReadFlow(std::size_t index, std::vector<float>& xy, FrameGenExtent& cells) override {
        if (index >= views_.size()) {
            return false;
        }
        std::vector<float> values;
        std::uint32_t channels = 0;
        if (!ReadTexture(index, {FrameGenTextureKind::kFilteredNewest, 0}, values, cells, channels) || channels != 4) {
            return false;
        }
        const FrameGenExtent extent = views_[index].history_state.Extent();
        xy.resize(std::size_t{cells.width} * cells.height * 2);
        for (std::size_t i = 0; i < std::size_t{cells.width} * cells.height; ++i) {
            xy[i * 2] = values[i * 4] * extent.width;
            xy[i * 2 + 1] = values[i * 4 + 1] * extent.height;
        }
        return true;
    }

    bool ReadTexture(std::size_t index, FrameGenTexture kind, std::vector<float>& values, FrameGenExtent& extent,
                     std::uint32_t& channels) override {
        if (index >= views_.size() || !WaitIdle()) {
            return false;
        }
        ViewResources& view = views_[index];
        // Ring slots moved on after the Execute that wrote them.
        switch (kind.kind) {
        case FrameGenTextureKind::kHistoryNewest: kind.kind = FrameGenTextureKind::kHistoryPrevious; break;
        case FrameGenTextureKind::kFilteredNewest: kind.kind = FrameGenTextureKind::kFilteredPrevious; break;
        case FrameGenTextureKind::kTrustNewest: kind.kind = FrameGenTextureKind::kTrustPrevious; break;
        case FrameGenTextureKind::kDepth: kind.kind = FrameGenTextureKind::kDepthPrevious; break;
        case FrameGenTextureKind::kFilteredCover: view.bound_newest = view.history_state.Previous(); break;
        default: break;
        }
        Texture* texture = Resolve(view, kind);
        view.bound_newest = -1;
        if (!texture || !texture->resource) {
            return false;
        }
        Texture& flow = *texture;
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
        if (FAILED(SubmitWork(list.Get())) || !WaitIdle()) {
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
        extent = {static_cast<std::uint32_t>(desc.Width), desc.Height};
        switch (desc.Format) {
        case DXGI_FORMAT_R16G16B16A16_FLOAT: channels = 4; break;
        case DXGI_FORMAT_R32G32B32A32_FLOAT: channels = 4; break;
        case DXGI_FORMAT_R32_FLOAT: channels = 1; break;
        case DXGI_FORMAT_R16_FLOAT: channels = 1; break;
        case DXGI_FORMAT_R16G16_FLOAT: channels = 2; break;
        case DXGI_FORMAT_R32_UINT: channels = 2; break; // two packed halves
        case DXGI_FORMAT_R8_UNORM: channels = 1; break;
        default: channels = 0; break;
        }
        if (channels == 0) {
            readback->Unmap(0, nullptr);
            return false;
        }
        values.resize(std::size_t{extent.width} * extent.height * channels);
        for (std::uint32_t y = 0; y < extent.height; ++y) {
            const auto* row = static_cast<const std::byte*>(mapped) + footprint.Offset + y * footprint.Footprint.RowPitch;
            float* out = values.data() + std::size_t{y} * extent.width * channels;
            for (std::uint32_t x = 0; x < extent.width; ++x) {
                switch (desc.Format) {
                case DXGI_FORMAT_R16G16B16A16_FLOAT: {
                    const auto* texel = reinterpret_cast<const std::uint16_t*>(row) + x * 4;
                    for (int c = 0; c < 4; ++c) out[x * 4 + c] = half(texel[c]);
                    break;
                }
                case DXGI_FORMAT_R32G32B32A32_FLOAT:
                    std::memcpy(&out[x * 4], row + x * 16, 4 * sizeof(float));
                    break;
                case DXGI_FORMAT_R32_FLOAT:
                    std::memcpy(&out[x], row + x * 4, sizeof(float));
                    break;
                case DXGI_FORMAT_R16_FLOAT: {
                    std::uint16_t texel = 0;
                    std::memcpy(&texel, row + x * 2, sizeof(texel));
                    out[x] = half(texel);
                    break;
                }
                case DXGI_FORMAT_R16G16_FLOAT: {
                    std::uint16_t texel[2]{};
                    std::memcpy(texel, row + x * 4, sizeof(texel));
                    out[x * 2] = half(texel[0]);
                    out[x * 2 + 1] = half(texel[1]);
                    break;
                }
                case DXGI_FORMAT_R32_UINT: {
                    std::uint32_t packed = 0;
                    std::memcpy(&packed, row + x * 4, sizeof(packed));
                    if (kind.kind == FrameGenTextureKind::kShiftKeys) {
                        // Splat keys: the bits above the low six are a float.
                        const std::uint32_t bits = packed & ~63u;
                        float value = 0.0f;
                        std::memcpy(&value, &bits, sizeof(value));
                        out[x * 2] = value;
                        out[x * 2 + 1] = static_cast<float>(packed & 63u);
                    } else {
                        out[x * 2] = half(static_cast<std::uint16_t>(packed & 0xffffu));
                        out[x * 2 + 1] = half(static_cast<std::uint16_t>(packed >> 16));
                    }
                    break;
                }
                default:
                    out[x] = static_cast<float>(static_cast<std::uint8_t>(row[x])) / 255.0f;
                    break;
                }
            }
        }
        readback->Unmap(0, nullptr);
        return true;
    }

    void SetDirectOutput(bool on) override { direct_output_ = on; }
    bool DrawsOutputs() const override { return direct_output_ && rtv_heap_; }

    // A wide view's periphery is rendered far denser than a headset shows
    // it: angle per pixel shrinks with the square of the angle's tangent off
    // the view axis, and lenses spread the panel's pixels out there. Beyond
    // this tangent (45 degrees) generated frames are shaded once per 2x2
    // pixels where no cockpit content is (FG_PASS_SHADING_RATE): cockpit
    // edges stay per pixel everywhere. Narrow views (focus insets) never
    // reach it.
    static constexpr double kCoarseShadingTangent = 1.0;
    double coarse_tangent_{kCoarseShadingTangent};
    DWORD wait_scale_{1}; // fence waits on a software rasterizer are this many times longer

    // Records the view's shading-rate image for the synthesis draw from this
    // frame's depth (`depth`, null when not in use: the angle alone decides).
    // True when the draw should use it.
    bool RecordShadingRate(ViewResources& view, const FrameGenViewSubmission& submission, Texture* depth,
                           std::string& error) {
        if (!shading_tile_) {
            return false;
        }
        const FrameGenExtent extent = submission.request.extent;
        const FrameGenViewGeometry& geometry = submission.request.newest;
        const std::array<float, 4> fov{geometry.angle_left, geometry.angle_right, geometry.angle_up, geometry.angle_down};
        const std::array<float, 4> tangents{std::tan(fov[0]), std::tan(fov[1]), std::tan(fov[2]), std::tan(fov[3])};
        if (view.shading_extent.width != extent.width || view.shading_extent.height != extent.height ||
            !std::equal(fov.begin(), fov.end(), view.shading_fov.begin(),
                        [](float a, float b) { return std::abs(a - b) < 1e-3f; })) {
            // Whether any corner reaches the angle (the tangent radius grows
            // toward the corners of the tangent rectangle).
            view.shading_extent = extent;
            view.shading_fov = fov;
            view.shading_coarse = false;
            for (const float x : {tangents[0], tangents[1]}) {
                for (const float y : {tangents[2], tangents[3]}) {
                    view.shading_coarse = view.shading_coarse || std::hypot(x, y) >= coarse_tangent_;
                }
            }
        }
        if (!view.shading_coarse) {
            return false;
        }
        const UINT tiles_x = (extent.width + shading_tile_ - 1) / shading_tile_;
        const UINT tiles_y = (extent.height + shading_tile_ - 1) / shading_tile_;
        if (!view.shading_rate.resource || view.shading_rate.resource->GetDesc().Width != tiles_x ||
            view.shading_rate.resource->GetDesc().Height != tiles_y) {
            if (!Create(view.shading_rate, {tiles_x, tiles_y}, DXGI_FORMAT_R8_UINT, DXGI_FORMAT_R8_UINT, true, error)) {
                view.shading_coarse = false;
                return false;
            }
        }
        if (descriptor_cursor_ + kDescriptorsPerDispatch > descriptor_end_) {
            error = "D3D12 Frame Generation descriptor budget exhausted";
            return false;
        }
        const UINT base = descriptor_cursor_;
        descriptor_cursor_ += kDescriptorsPerDispatch;
        std::vector<D3D12_RESOURCE_BARRIER> barriers;
        ShaderResource(depth, base, 0);
        if (depth) Require(barriers, *depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        for (UINT i = 1; i < kShaderResources; ++i) {
            ShaderResource(nullptr, base, i);
        }
        UnorderedAccess(&view.shading_rate, base + kFirstUav);
        UnorderedAccess(nullptr, base + kFirstUav + 1);
        Require(barriers, view.shading_rate, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Flush(barriers);
        FrameGenConstants constants{};
        constants.fov_current = tangents;
        constants.size = {tiles_x, tiles_y, extent.width, extent.height};
        constants.options = {shading_tile_, depth ? kFrameGenFlagDepth : 0u, 0u, 0u};
        constants.tuning = {static_cast<float>(coarse_tangent_), 0.0f, 0.0f, 0.0f};
        commands_->SetPipelineState(pipelines_[static_cast<std::size_t>(FrameGenPass::kShadingRate)].Get());
        commands_->SetComputeRoot32BitConstants(0, sizeof(FrameGenConstants) / 4, &constants, 0);
        commands_->SetComputeRootDescriptorTable(1, Gpu(base));
        commands_->SetComputeRootDescriptorTable(2, Gpu(base + kFirstUav));
        commands_->Dispatch((tiles_x + kFrameGenThreadGroupSize - 1) / kFrameGenThreadGroupSize,
                            (tiles_y + kFrameGenThreadGroupSize - 1) / kFrameGenThreadGroupSize, 1);
        Stamp(static_cast<int>(FrameGenPass::kShadingRate));
        return true;
    }

    // Draws generated frames (the plan's synthesis dispatches, one per
    // output) straight into the output images in one draw: `indices` are
    // the outputs, `frames` their dispatches.
    bool RecordDraw(ViewResources& view, const FrameGenViewSubmission& submission,
                    const std::vector<ID3D12Resource*>& outputs, const std::vector<std::size_t>& indices,
                    const std::vector<const FrameGenDispatch*>& frames, std::string& error) {
        std::array<DXGI_FORMAT, kFrameGenMaxGenerated> formats{};
        for (std::size_t i = 0; i < frames.size(); ++i) {
            formats[i] = static_cast<DXGI_FORMAT>(submission.outputs[indices[i]].format);
        }
        ID3D12PipelineState* pipeline = DrawPipeline(formats, frames.size());
        if (!pipeline) {
            error = "D3D12 Frame Generation draw pipeline unavailable";
            return false;
        }
        const FrameGenDispatch& first = *frames[0];
        // Its own dispatch first, its barriers flushed: the synthesis input
        // t3 is the newest frame's depth when it is in use. Without the image
        // the frames are drawn per pixel.
        std::string rate_error;
        const bool still = (first.constants.options[1] & kFrameGenFlagStill) != 0u;
        const bool coarse =
            !still && coarse_shading_ && RecordShadingRate(view, submission, Resolve(view, first.inputs[3]), rate_error);
        if (descriptor_cursor_ + kDescriptorsPerDispatch > descriptor_end_) {
            error = "D3D12 Frame Generation descriptor budget exhausted";
            return false;
        }
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
        if (coarse) Require(barriers, view.shading_rate, D3D12_RESOURCE_STATE_SHADING_RATE_SOURCE);
        Flush(barriers);

        // Output images are in RENDER_TARGET while acquired.
        std::array<D3D12_CPU_DESCRIPTOR_HANDLE, kFrameGenMaxGenerated> targets{};
        for (std::size_t i = 0; i < frames.size(); ++i) {
            const FrameGenImage& image = submission.outputs[indices[i]];
            ID3D12Resource* output = outputs[indices[i]];
            D3D12_RENDER_TARGET_VIEW_DESC rtv{};
            rtv.Format = formats[i];
            if (output->GetDesc().DepthOrArraySize > 1) {
                rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
                rtv.Texture2DArray.FirstArraySlice = image.array_index;
                rtv.Texture2DArray.ArraySize = 1;
            } else {
                rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
            }
            targets[i] = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
            targets[i].ptr += static_cast<SIZE_T>(rtv_cursor_++ % kRenderTargetViews) * rtv_size_;
            device_->CreateRenderTargetView(output, &rtv, targets[i]);
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
        if (coarse) {
            const D3D12_SHADING_RATE_COMBINER combiners[2]{D3D12_SHADING_RATE_COMBINER_PASSTHROUGH,
                                                           D3D12_SHADING_RATE_COMBINER_OVERRIDE};
            commands5_->RSSetShadingRate(D3D12_SHADING_RATE_1X1, combiners);
            commands5_->RSSetShadingRateImage(view.shading_rate.resource.Get());
        }
        commands_->DrawInstanced(3, 1, 0, 0);
        if (coarse) {
            commands5_->RSSetShadingRateImage(nullptr);
            commands5_->RSSetShadingRate(D3D12_SHADING_RATE_1X1, nullptr);
        }
        Stamp(static_cast<int>(FrameGenPass::kSynthesizeUnorm8));
        return true;
    }

    // Profiling: what ran since the previous stamp.
    static constexpr int kProfileSourceCopy = static_cast<int>(kFrameGenPassCount);
    static constexpr int kProfileOutputCopy = kProfileSourceCopy + 1;
    static constexpr int kProfileHardware = kProfileOutputCopy + 1; // waiting for the optical flow hardware
    static constexpr int kProfileCompose = kProfileHardware + 1;   // quad views: composing an eye
    static constexpr int kProfileFlowMerge = kProfileCompose + 1;  // quad views: the flow merge
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
        profile_ms_.assign(kProfileFlowMerge + 1, 0.0);
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
        HRESULT hr = DeviceRemovedReason();
        if (FAILED(hr)) {
            error = Failure("D3D12 device", hr);
            return false;
        }
        if (hardware_stuck_.exchange(false)) {
            HardwareFailed("the optical flow hardware stopped responding");
        }
        // Engine motion vectors still waiting for their color (released
        // before it, CaptureMotion's `pair_next`) are a frame whose color was
        // never captured: the game releases a frame's images before its
        // xrEndFrame, which runs this. They pair with no later frame's color.
        for (ViewResources& view : views_) {
            view.engine_pair_next = false;
        }
        if (Uncaptured(views, tuning, result, error)) {
            return false;
        }
        source_copied_ = false;
        const std::size_t slot_index = static_cast<std::size_t>(submission_ % kFramesInFlight);
        FrameSlot& slot = slots_[slot_index];
        if (!WaitForSlot(slot, error)) {
            return false;
        }
        ReadTiming(slot, slot_index);
        if (slot.analysis_pending) {
            CollectTiming(slot, slot_index); // before its stamps are written again
        }
        slot.hardware_timed = false;
        slot.hardware_call_ms = 0.0;
        ReadDepthCheck(slot);
        ReadEngineEvidence(slot);
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
        if (timed) {
            commands_->EndQuery(query_heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, static_cast<UINT>(slot_index * kTimingStamps));
        }
        profile_labels_.clear();
        if (profiling_) commands_->EndQuery(profile_heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);

        TrimViews(views.size());
        if (views_.size() < views.size()) {
            views_.resize(views.size());
        }
        FrameGenTuning effective_tuning = tuning;
        effective_tuning.occlusion = tuning.occlusion && occlusion_;
        effective_tuning.cross_fade = tuning.cross_fade || cross_fade_;
        effective_tuning.depth_match = tuning.depth_match || depth_match_;
        effective_tuning.depth_splat = tuning.depth_splat || depth_splat_;
        effective_tuning.hardware_flow = tuning.hardware_flow && HardwareFlowReady();
        // Engine motion vectors (stage 3): their evidence is taken with the
        // convention the validator has under test, or locked.
        if (tuning.engine_mode >= FrameGenEngineMode::kObserve) {
            effective_tuning.engine = engine_validator_.Convention();
            effective_tuning.engine_locked = engine_validator_.Locked();
        }
        // Several views on Frame Generation's own queue match on one atlas;
        // otherwise (one view, matching at release on the game's queue, or
        // the atlas refused) a session per view, its hints following that
        // view's motion.
        atlas_in_use_ = effective_tuning.hardware_flow && atlas_enabled_ && !atlas_failed_ && separate_ && views.size() >= 2;
        for (std::size_t i = 0; i < views.size() && effective_tuning.hardware_flow; ++i) {
            effective_tuning.hardware_flow = EnsureHardware(views_[i], i, views[i], tuning, !atlas_in_use_);
        }
        if (atlas_in_use_ && effective_tuning.hardware_flow && !EnsureAtlas(views, tuning)) {
            atlas_in_use_ = false;
            if (atlas_busy_) {
                // The GPU did not finish before an atlas switch: no refusal.
                // This frame measures on the shader cores; the next one
                // tries the switch again.
                atlas_busy_ = false;
                effective_tuning.hardware_flow = false;
            } else {
                atlas_failed_ = !hardware_failed_;
                effective_tuning.hardware_flow = !hardware_failed_;
                for (std::size_t i = 0; i < views.size() && effective_tuning.hardware_flow; ++i) {
                    effective_tuning.hardware_flow = EnsureHardware(views_[i], i, views[i], tuning, true);
                }
            }
        }
        // Captures start matching on the hardware while it is in use.
        hardware_active_ = effective_tuning.hardware_flow;
        // Measured on the shader cores meanwhile (Interpolate in Dynamic, or
        // the hardware not ready): the hardware's last vectors are from an
        // earlier stretch of frames, so its next match starts unhinted.
        if (!effective_tuning.hardware_flow) {
            for (auto& view : views_) view.hardware_fresh = true;
#ifdef DEPTHXR_NVOF
            atlas_.fresh = true;
#endif
        }
        bool ok = true;
        // A game that fills only one eye's depth (FrameGenDepthEyes): that
        // eye takes the other's, moved to its view, before the trust pass
        // and the plans read it.
        if (const std::optional<std::size_t> empty = eyes_.Empty(); empty && views.size() >= 2) {
            const std::size_t target = *empty;
            const std::size_t source = 1 - target;
            bool both = true;
            for (std::size_t i = 0; i < 2; ++i) {
                both = both && Prepared(views[i], views_[i], FrameGenHistoryRing(tuning)) &&
                       views_[i].capture.Matches(views[i].source);
            }
            if (both && views[source].request.depth && views[target].request.depth && views_[source].depth_ready &&
                views_[source].depth_generation == validator_.Generation()) {
                ok = RecordDepthCross(views[source], views[target], views_[source], views_[target], error);
                Stamp(static_cast<int>(FrameGenPass::kDepthCrossResolve));
            }
        }
        // Depth the other eye disproves is dropped per texel before any view
        // uses it. Needs both eyes' images of this frame, captured at release.
        if (ok && views.size() >= 2 && validator_.Usable()) {
            bool both = true;
            for (std::size_t i = 0; i < 2; ++i) {
                both = both && Prepared(views[i], views_[i], FrameGenHistoryRing(tuning)) &&
                       views_[i].capture.Matches(views[i].source);
            }
            for (std::size_t i = 0; i < 2 && both && ok; ++i) {
                ViewResources& view = views_[i];
                if (views[i].request.depth && view.depth_ready && view.depth_generation == validator_.Generation()) {
                    ok = RecordDepthTrust(views[i], views[1 - i], view, views_[1 - i], error);
                    Stamp(static_cast<int>(FrameGenPass::kDepthTrust));
                }
            }
        }
        // Engine motion vectors (stage 3): the evidence on every view's vectors
        // or on none (one eye judged and the other not would part the eyes
        // once they are used): each captured for this frame, the view kept as
        // it is (images, history) with its depth in use.
        bool evidence = tuning.engine_mode >= FrameGenEngineMode::kObserve && views.size() == kMotionViews &&
                        !engine_failed_ && engine_validator_.GetState() != FrameGenEngineValidator::State::kDisabled;
        for (std::size_t i = 0; i < views.size() && evidence; ++i) {
            const ViewResources& view = views_[i];
            const std::uint8_t newest = EngineNewest(view);
            evidence = views[i].request.engine_motion && Prepared(views[i], view, FrameGenHistoryRing(tuning)) &&
                       view.history_state.Effective(views[i].request).synthesize && view.engine_ready &&
                       view.engine_raw[newest].resource && view.engine_params.resource &&
                       view.engine_pair[newest].Matches(views[i].source) && views[i].request.depth && view.depth_ready &&
                       validator_.Usable() && view.depth_generation == validator_.Generation();
        }
        // Every view up to the optical flow hardware, the hardware for all of
        // them, then the rest (without hardware flow, everything at once).
        std::vector<ViewRecording> recordings(views.size());
        bool hardware = false;
        for (std::size_t i = 0; i < views.size() && ok; ++i) {
            bool synthesized = false;
            bool depth = false;
            ok = BeginView(views[i], effective_tuning, views_[i], recordings[i], synthesized, depth, evidence, error);
            result.synthesized[i] = synthesized;
            result.depth[i] = depth;
        }
        // Research dumps read the inputs before the plans change the depth.
        std::vector<DumpCopy> dump;
        if (ok && !dump_directory_.empty()) {
            ok = RecordDump(views, recordings, dump, error);
        }
        for (std::size_t i = 0; i < views.size() && ok; ++i) {
            ok = RecordPlan(recordings[i], effective_tuning.hardware_flow, error);
            hardware = hardware || recordings[i].waits_for_hardware;
        }
        if (ok && hardware) {
            ok = RunHardwareFlow(recordings, slot, slot_index, error);
            for (std::size_t i = 0; i < views.size() && ok; ++i) {
                ok = RecordPlan(recordings[i], false, error);
            }
        }
        // Quad views: each eye's periphery moves content as its focus view
        // does where the eye's image blends the two.
        if (ok && effective_tuning.analyze_only && effective_tuning.composition && flow_merge_pipeline_) {
            ok = RecordFlowMerge(*effective_tuning.composition, recordings, error);
        }
        // The engine evidence back to the CPU, for the validator.
        const bool evidence_recorded =
            ok && !recordings.empty() &&
            std::all_of(recordings.begin(), recordings.end(), [](const ViewRecording& r) { return r.evidence; }) &&
            RecordEngineReadback(slot, recordings, effective_tuning);
        // Check the game's depth against the other eye while undecided, and
        // now and then afterwards: from view 0, or from the eye whose depth
        // the game fills (FrameGenDepthEyes).
        const std::size_t check_source = eyes_.Source() == 1 ? 1 : 0;
        const std::size_t check_other = 1 - check_source;
        if (ok && views.size() >= 2 && views_[check_source].depth_fresh && views[check_source].request.depth &&
            views_[check_source].depth_generation == validator_.Generation() && validator_.WantsCheck(submission_)) {
            ok = RecordDepthCheck(views[check_source], views[check_other], views_[check_source], views_[check_other],
                                  slot, error);
            slot.check_source = check_source;
            Stamp(static_cast<int>(FrameGenPass::kDepthCheck));
        }
        for (std::size_t i = 0; i < views.size(); ++i) {
            views_[i].depth_fresh = false;
        }
        if (ok && effective_tuning.analyze_only) {
            // What frames written just in time read (on the present queue,
            // while the next measurement may run here): readable by every
            // shader stage from now until the slot is written again, so no
            // queue transitions it while the other reads it.
            std::vector<D3D12_RESOURCE_BARRIER> barriers;
            for (std::size_t i = 0; i < views.size(); ++i) {
                ViewResources& view = views_[i];
                const std::uint8_t newest = view.history_state.Newest();
                for (Texture* texture : {&view.history[newest], &view.filtered[newest], &view.filtered_cover[newest],
                                         &view.depth[newest], &view.history[view.history_state.Previous()]}) {
                    if (texture->resource) Require(barriers, *texture, D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE);
                }
            }
            Flush(barriers);
        }
        if (timed) {
            const UINT first = static_cast<UINT>(slot_index * kTimingStamps);
            commands_->EndQuery(query_heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, first + kTimingStamps - 1);
            // The stamps around the optical flow hardware only when it ran.
            if (slot.hardware_timed) {
                commands_->ResolveQueryData(query_heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, first, kTimingStamps,
                                            readback_.Get(), first * sizeof(UINT64));
            } else {
                commands_->ResolveQueryData(query_heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, first, 1, readback_.Get(),
                                            first * sizeof(UINT64));
                commands_->ResolveQueryData(query_heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, first + kTimingStamps - 1, 1,
                                            readback_.Get(), (first + kTimingStamps - 1) * sizeof(UINT64));
            }
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
            analyzed_.clear();
            return false;
        }
        slot.timed_submission = 0;
        if (!effective_tuning.analyze_only && present_fence_ && present_.Get() != work_.Get()) {
            // Output images the runtime's queue may still be reading (it
            // synchronizes their reuse there, not here).
            present_->Signal(present_fence_.Get(), ++present_value_);
            work_->Wait(present_fence_.Get(), present_value_);
        }
        if (timed && SUCCEEDED(work_->GetClockCalibration(&slot.calibration_gpu, &slot.calibration_cpu))) {
            slot.timed_submission = submission_ + 1;
        }
        // An analysis is timed from the newest capture it waits for (none
        // when it copied an image itself), whose stamps the game's queue took.
        slot.capture_mark = source_copied_ ? 0 : last_capture_value_;
        if (effective_tuning.analyze_only && slot.timed_submission && capture_query_heap_ &&
            FAILED(queue_->GetClockCalibration(&capture_calibration_gpu_, &capture_calibration_cpu_))) {
            capture_calibration_gpu_ = capture_calibration_cpu_ = 0;
        }
        result.submission = slot.timed_submission;
        hr = SubmitWork(commands_.Get());
        for (std::size_t i = 0; i < views.size(); ++i) {
            MarkUsed(views_[i], fence_.Get(), fence_value_); // submitted even when its signal failed
        }
        if (FAILED(hr)) {
            error = Failure("D3D12 Frame Generation queue signal", hr);
            signal_failed_ = true;
            return false;
        }
        slot.fence_value = fence_value_;
        slot.timing_pending = timed;
        slot.analysis_pending = effective_tuning.analyze_only && slot.timed_submission != 0;
        slot.engine_pending = evidence_recorded;
        ++submission_;
        if (!effective_tuning.analyze_only || source_copied_) {
            // Outputs written, or an application image read (SetCopyAfterRelease):
            // the game's queue continues once this work is done with them.
            HandOverOutputs();
        }
        source_copied_ = false;
        // The next capture into these slots waits for this work on the GPU,
        // and so does the engine motion capture into the frame's own raw
        // field, two captures later (dumped in stage 2), and from Observe
        // into the field before (its evidence reads both, and their
        // censuses): with three fields, the capture after next.
        for (std::size_t i = 0; i < views.size(); ++i) {
            ViewResources& view = views_[i];
            view.slot_read[view.history_state.Newest()] = view.slot_read[view.history_state.Previous()] = fence_value_;
            if (recordings[i].engine) {
                view.engine_read[EngineNewest(view)] = fence_value_;
            }
            if (recordings[i].evidence) {
                view.engine_read[EnginePrevious(view)] = fence_value_;
            }
        }
        if (effective_tuning.analyze_only) {
            // What Synthesize writes this real frame's generated frames from.
            AnalyzedFrame frame;
            frame.id = next_analyzed_id_++;
            frame.tuning = effective_tuning;
            frame.tuning.composition = nullptr; // the caller's, for this call only
            frame.work_value = fence_value_;
            // Captures run on the game's queue: with a work queue of its own,
            // the capture fence this work waited for; else the work itself.
            frame.capture_value = separate_ ? capture_waited_ : fence_value_;
            for (std::size_t i = 0; i < views.size(); ++i) {
                const ViewResources& view = views_[i];
                AnalyzedView analyzed;
                analyzed.valid = true;
                analyzed.newest = view.history_state.Newest();
                analyzed.previous = view.history_state.Previous();
                analyzed.newest_generation = view.slot_generation[analyzed.newest];
                analyzed.previous_generation = view.slot_generation[analyzed.previous];
                analyzed.request = recordings[i].request;
                analyzed.request.synthesize = recordings[i].plan.synthesize;
                analyzed.cover = effective_tuning.cover && effective_tuning.mode == FrameGenSynthesisMode::kInterpolate &&
                                 recordings[i].plan.synthesize;
                frame.views.push_back(analyzed);
            }
            analyzed_.push_back(std::move(frame));
            // No more than the ring holds images of.
            while (analyzed_.size() > FrameGenHistoryRing(effective_tuning)) {
                analyzed_.pop_front();
            }
            result.frame = analyzed_.back().id;
        }
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

    bool SupportsJustInTime() const override { return just_in_time_; }

    bool Measured(std::uint64_t frame_id) const override {
        const AnalyzedFrame* found = FindAnalyzed(frame_id);
        return !found || !fence_ || fence_->GetCompletedValue() >= found->work_value;
    }

    bool Captured(std::uint64_t frame_id) const override {
        const AnalyzedFrame* found = FindAnalyzed(frame_id);
        ID3D12Fence* fence = CaptureFence();
        return !found || !fence || fence->GetCompletedValue() >= found->capture_value;
    }

    bool Synthesize(std::uint64_t frame_id, FrameGenSynthesisMode mode, float fraction,
                    const std::vector<FrameGenImage>& outputs, std::string& error) override {
        HRESULT hr = DeviceRemovedReason();
        if (FAILED(hr)) {
            error = Failure("D3D12 device", hr);
            return false;
        }
        // A frame at the newest frame's own time is a copy of it alone.
        const bool exact = mode == FrameGenSynthesisMode::kExtrapolate ? fraction <= 0.0f : fraction >= 1.0f;
        const AnalyzedFrame* kept = KeptFrame(frame_id, exact, error);
        if (!kept) {
            return false;
        }
        const AnalyzedFrame& frame = *kept;
        if (outputs.size() < frame.views.size()) {
            error = "the real frame is no longer kept";
            return false;
        }
        // Set up before anything is recorded: a refusal leaves the views and
        // their analyzed frames as they are.
        std::vector<FrameGenViewSubmission> submissions(frame.views.size());
        std::vector<ViewRecording> recordings(frame.views.size());
        for (std::size_t i = 0; i < frame.views.size(); ++i) {
            const AnalyzedView& analyzed = frame.views[i];
            const auto output_it = images_.find(outputs[i].swapchain);
            if (output_it == images_.end() || outputs[i].image_index >= output_it->second.size()) {
                error = "Frame Generation output image is not tracked";
                return false;
            }
            FrameGenViewSubmission& submission = submissions[i];
            submission.outputs = {outputs[i]};
            submission.request = analyzed.request;
            ViewRecording& recording = recordings[i];
            recording.submission = &submission;
            recording.view = &views_[i];
            recording.request = analyzed.request;
            recording.outputs = {output_it->second[outputs[i].image_index]};
            recording.plan = BuildFrameGenSynthesisPlan(analyzed.request, frame.tuning, mode, fraction, analyzed.cover);
            if (!PrepareFrame(recording, error)) {
                return false;
            }
        }
        return RecordSynthesis(
            frame, exact,
            [&](std::string& record_error) {
                for (std::size_t i = 0; i < frame.views.size(); ++i) {
                    if (!RecordFrame(recordings[i], frame.views[i], record_error)) {
                        return false;
                    }
                }
                return true;
            },
            error);
    }

    bool SynthesizeStale(std::uint64_t frame_id, std::uint64_t motion_id, float fraction,
                         const std::vector<FrameGenImage>& outputs, std::string& error) override {
        HRESULT hr = DeviceRemovedReason();
        if (FAILED(hr)) {
            error = Failure("D3D12 Frame Generation device", hr);
            return false;
        }
        // The frame's newest and previous images (its previous slot holds
        // `motion`'s newest image and filtered flow).
        const AnalyzedFrame* kept = KeptFrame(frame_id, false, error);
        if (!kept) {
            return false;
        }
        const AnalyzedFrame& frame = *kept;
        const AnalyzedFrame* motion = FindAnalyzed(motion_id);
        if (!motion || motion->views.size() != frame.views.size() || !ImagesKept(*motion, true) ||
            fence_->GetCompletedValue() < motion->work_value || outputs.size() < frame.views.size()) {
            error = "the previous real frame's motion is not kept or not measured";
            return false;
        }
        std::vector<FrameGenViewSubmission> submissions(frame.views.size());
        std::vector<ViewRecording> recordings(frame.views.size());
        for (std::size_t i = 0; i < frame.views.size(); ++i) {
            const AnalyzedView& analyzed = frame.views[i];
            const AnalyzedView& before = motion->views[i];
            // Its filtered flow is the one in this frame's previous slot, and
            // was measured from a pair (it had a previous frame itself).
            if (!analyzed.request.synthesize || !before.valid || !before.request.synthesize ||
                before.newest != analyzed.previous || before.newest_generation != analyzed.previous_generation) {
                error = "the previous real frame's motion is not this frame's previous";
                return false;
            }
            // Shown from where the eye is at its time, like the frames its own
            // motion moves: its eye shift reads the previous frame's depth.
            if (analyzed.request.depth && !analyzed.request.depth_previous) {
                error = "the previous real frame's depth is not resolved";
                return false;
            }
            const auto output_it = images_.find(outputs[i].swapchain);
            if (output_it == images_.end() || outputs[i].image_index >= output_it->second.size()) {
                error = "Frame Generation output image is not tracked";
                return false;
            }
            FrameGenViewSubmission& submission = submissions[i];
            submission.outputs = {outputs[i]};
            submission.request = analyzed.request;
            ViewRecording& recording = recordings[i];
            recording.submission = &submission;
            recording.view = &views_[i];
            recording.request = analyzed.request;
            recording.outputs = {output_it->second[outputs[i].image_index]};
            recording.plan = BuildFrameGenSynthesisPlan(analyzed.request, motion->tuning, FrameGenSynthesisMode::kExtrapolate,
                                                        fraction, false, true);
            if (!PrepareFrame(recording, error)) {
                return false;
            }
        }
        // Waits for the frame's capture alone, as its exact copy does: what
        // it reads of `motion` was measured already, and every texture read
        // stays readable by any queue until its slot is written again.
        return RecordSynthesis(
            frame, true,
            [&](std::string& record_error) {
                for (std::size_t i = 0; i < frame.views.size(); ++i) {
                    if (!RecordFrame(recordings[i], frame.views[i], record_error)) {
                        return false;
                    }
                }
                return true;
            },
            error);
    }

    bool SupportsComposition() const override { return supports_composition_; }

    bool SynthesizeComposed(std::uint64_t frame_id, FrameGenSynthesisMode mode, float fraction,
                            const FrameGenComposition& composition, const std::vector<FrameGenImage>& outputs,
                            std::string& error) override {
        return ComposeSynthesis(frame_id, 0, mode, fraction, composition, outputs, error);
    }

    bool SynthesizeComposedStale(std::uint64_t frame_id, std::uint64_t motion_id, float fraction,
                                 const FrameGenComposition& composition, const std::vector<FrameGenImage>& outputs,
                                 std::string& error) override {
        return motion_id && ComposeSynthesis(frame_id, motion_id, FrameGenSynthesisMode::kExtrapolate, fraction,
                                             composition, outputs, error);
    }

    // SynthesizeComposed, or with `motion_id` SynthesizeComposedStale (the
    // checks and waits of SynthesizeStale).
    bool ComposeSynthesis(std::uint64_t frame_id, std::uint64_t motion_id, FrameGenSynthesisMode mode, float fraction,
                          const FrameGenComposition& composition, const std::vector<FrameGenImage>& outputs,
                          std::string& error) {
        if (!supports_composition_) {
            error = "this Direct3D 12 device cannot compose quad views";
            return false;
        }
        HRESULT hr = DeviceRemovedReason();
        if (FAILED(hr)) {
            error = Failure("D3D12 device", hr);
            return false;
        }
        const bool stale = motion_id != 0;
        const bool exact = !stale && (mode == FrameGenSynthesisMode::kExtrapolate ? fraction <= 0.0f : fraction >= 1.0f);
        const AnalyzedFrame* kept = KeptFrame(frame_id, exact, error);
        if (!kept) {
            return false;
        }
        const AnalyzedFrame& frame = *kept;
        const std::size_t count = frame.views.size();
        const AnalyzedFrame* motion = stale ? FindAnalyzed(motion_id) : nullptr;
        if (stale && (!motion || motion->views.size() != count || !ImagesKept(*motion, true) ||
                      fence_->GetCompletedValue() < motion->work_value)) {
            error = "the previous real frame's motion is not kept or not measured";
            return false;
        }
        for (std::size_t i = 0; stale && i < count; ++i) {
            const AnalyzedView& analyzed = frame.views[i];
            const AnalyzedView& before = motion->views[i];
            if (!analyzed.request.synthesize || !before.valid || !before.request.synthesize ||
                before.newest != analyzed.previous || before.newest_generation != analyzed.previous_generation) {
                error = "the previous real frame's motion is not this frame's previous";
                return false;
            }
            if (analyzed.request.depth && !analyzed.request.depth_previous) {
                error = "the previous real frame's depth is not resolved";
                return false;
            }
        }
        if (composition.eyes.empty() || outputs.size() < composition.eyes.size()) {
            error = "a composed eye has no output image";
            return false;
        }
        for (const FrameGenComposeEye& eye : composition.eyes) {
            if (eye.peripheral >= count || eye.focus >= count) {
                error = "a composed eye names a view the real frame does not have";
                return false;
            }
        }
        // Each view's frame at this time: it moves content, or it is the real
        // frame's own (an exact copy, a still, a view without motion).
        std::vector<ViewRecording> recordings(count);
        std::vector<bool> moved(count, false);
        for (std::size_t i = 0; i < count; ++i) {
            const AnalyzedView& analyzed = frame.views[i];
            recordings[i].plan = stale ? BuildFrameGenSynthesisPlan(analyzed.request, motion->tuning, mode, fraction,
                                                                    false, true)
                                       : BuildFrameGenSynthesisPlan(analyzed.request, frame.tuning, mode, fraction,
                                                                    analyzed.cover);
            moved[i] = recordings[i].plan.synthesize;
        }
        // An eye whose views disagree shows the real frame in both, so the two
        // line up where the eye blends them; a periphery is shaded per pixel
        // (the compose magnifies it), and a focus view limits the motion its
        // periphery would: it spans the rectangle's width of the periphery.
        std::vector<bool> eye_moved(composition.eyes.size(), false);
        std::vector<bool> warped(count, false), periphery(count, false), limited(count, false);
        for (std::size_t e = 0; e < composition.eyes.size(); ++e) {
            const FrameGenComposeEye& eye = composition.eyes[e];
            eye_moved[e] = moved[eye.peripheral] && moved[eye.focus];
            periphery[eye.peripheral] = true;
            if (!eye_moved[e]) {
                continue;
            }
            warped[eye.peripheral] = warped[eye.focus] = true;
            if (!limited[eye.focus]) {
                limited[eye.focus] = true;
                const float width = std::max(eye.focus_rect[2] - eye.focus_rect[0], 0.0001f);
                for (FrameGenDispatch& dispatch : recordings[eye.focus].plan.dispatches) {
                    if (Generates(dispatch)) dispatch.constants.tuning[1] /= width;
                }
            }
        }
        // Everything that can be refused is set up before anything is
        // recorded, so a refusal (an image that cannot be created or drawn
        // into) leaves the views and their analyzed frames as they are: each
        // moved view's compose target and frame, then each eye's output.
        std::vector<FrameGenViewSubmission> submissions(count);
        for (std::size_t i = 0; i < count; ++i) {
            if (!warped[i]) {
                continue;
            }
            const AnalyzedView& analyzed = frame.views[i];
            ViewResources& view = views_[i];
            if (!EnsureComposeTarget(view, analyzed.request, error)) {
                return false;
            }
            FrameGenViewSubmission& submission = submissions[i];
            const XrRect2Di whole{{0, 0},
                                  {static_cast<std::int32_t>(analyzed.request.extent.width),
                                   static_cast<std::int32_t>(analyzed.request.extent.height)}};
            submission.outputs = {
                FrameGenImage{XR_NULL_HANDLE, 0, 0, whole, static_cast<std::int64_t>(view.compose_target.view_format)}};
            submission.request = analyzed.request;
            ViewRecording& recording = recordings[i];
            recording.submission = &submission;
            recording.view = &view;
            recording.request = analyzed.request;
            recording.outputs = {view.compose_target.resource.Get()};
            if (!PrepareFrame(recording, error)) {
                return false;
            }
        }
        std::vector<ID3D12Resource*> eye_outputs(composition.eyes.size(), nullptr);
        for (std::size_t e = 0; e < composition.eyes.size(); ++e) {
            if (!ComposeOutput(outputs[e], composition.output, eye_outputs[e], error)) {
                return false;
            }
        }
        // Stale: the frame's capture alone is waited for, as its exact copy.
        return RecordSynthesis(
            frame, exact || stale,
            [&](std::string& record_error) {
                std::vector<D3D12_RESOURCE_BARRIER> barriers;
                for (std::size_t i = 0; i < count; ++i) {
                    if (!warped[i]) {
                        continue;
                    }
                    ViewResources& view = views_[i];
                    // Drawn into like an output image (RecordDraw does not
                    // transition those).
                    Require(barriers, view.compose_target, D3D12_RESOURCE_STATE_RENDER_TARGET);
                    Flush(barriers);
                    coarse_shading_ = !periphery[i];
                    const bool recorded = RecordFrame(recordings[i], frame.views[i], record_error);
                    coarse_shading_ = true;
                    if (!recorded) {
                        return false;
                    }
                }
                for (std::size_t e = 0; e < composition.eyes.size(); ++e) {
                    const FrameGenComposeEye& eye = composition.eyes[e];
                    auto source = [&](std::uint32_t index) -> Texture& {
                        ViewResources& view = views_[index];
                        return eye_moved[e] ? view.compose_target : view.history[frame.views[index].newest];
                    };
                    if (!RecordCompose(source(eye.peripheral), source(eye.focus), outputs[e], eye_outputs[e],
                                       BuildFrameGenComposeConstants(eye, composition.output), composition.output,
                                       record_error)) {
                        return false;
                    }
                }
                return true;
            },
            error);
    }

    std::uint64_t CaptureMark() const override { return last_capture_value_; }

    bool CaptureReached(std::uint64_t mark) const override {
        ID3D12Fence* fence = CaptureFence();
        return !mark || !fence || fence->GetCompletedValue() >= mark;
    }

    // Waits in slices of 10 ms at most on an event of its own: a removed
    // device or a signal that failed (the fence would never get there) ends
    // the wait at once, as reached.
    bool WaitCapture(std::uint64_t mark, std::chrono::milliseconds timeout) const override {
        ID3D12Fence* fence = CaptureFence();
        if (!mark || !fence || fence->GetCompletedValue() >= mark) {
            return true;
        }
        if (signal_failed_ || !capture_wait_event_ || FAILED(fence->SetEventOnCompletion(mark, capture_wait_event_))) {
            return true;
        }
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (fence->GetCompletedValue() < mark) {
            if (signal_failed_ || FAILED(device_->GetDeviceRemovedReason())) {
                return true;
            }
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) {
                return false;
            }
            const auto left = std::chrono::ceil<std::chrono::milliseconds>(deadline - now).count();
            WaitForSingleObject(capture_wait_event_, static_cast<DWORD>(std::min<long long>(left, 10)));
        }
        return true;
    }

    std::vector<FrameGenAnalysisTiming> TakeTimings() override {
        for (std::size_t i = 0; i < slots_.size(); ++i) {
            if (slots_[i].analysis_pending && fence_ && fence_->GetCompletedValue() >= slots_[i].fence_value) {
                CollectTiming(slots_[i], i);
            }
        }
        std::sort(timings_.begin(), timings_.end(),
                  [](const FrameGenAnalysisTiming& a, const FrameGenAnalysisTiming& b) { return a.submission < b.submission; });
        std::vector<FrameGenAnalysisTiming> taken;
        taken.swap(timings_);
        return taken;
    }

    // An analysis' GPU timestamps (its slot's fence has passed), for
    // TakeTimings; the oldest are dropped while nobody takes them.
    void CollectTiming(FrameSlot& slot, std::size_t slot_index) {
        slot.analysis_pending = false;
        LARGE_INTEGER frequency{};
        if (!readback_ || !timestamp_frequency_ || !QueryPerformanceFrequency(&frequency)) {
            return;
        }
        const D3D12_RANGE range{slot_index * kTimingStamps * sizeof(UINT64), (slot_index + 1) * kTimingStamps * sizeof(UINT64)};
        void* mapped = nullptr;
        if (FAILED(readback_->Map(0, &range, &mapped))) {
            return;
        }
        UINT64 stamps[kTimingStamps]{};
        std::memcpy(stamps, static_cast<const std::byte*>(mapped) + range.Begin, sizeof(stamps));
        const D3D12_RANGE written{0, 0};
        readback_->Unmap(0, &written);
        if (stamps[kTimingStamps - 1] < stamps[0]) {
            return;
        }
        auto clock = [&](UINT64 stamp) {
            return SteadyTime(stamp, slot.calibration_gpu, slot.calibration_cpu, timestamp_frequency_, frequency.QuadPart);
        };
        FrameGenAnalysisTiming timing;
        timing.submission = slot.timed_submission;
        timing.capture_end = CaptureEnd(slot.capture_mark);
        timing.start = clock(stamps[0]);
        if (slot.hardware_timed) {
            timing.hardware_start = clock(stamps[1]);
            timing.hardware_end = clock(stamps[2]);
            timing.hardware_call_ms = slot.hardware_call_ms;
        }
        timing.end = clock(stamps[kTimingStamps - 1]);
        constexpr std::size_t kKeptTimings = 16;
        if (timings_.size() >= kKeptTimings) {
            timings_.erase(timings_.begin());
        }
        timings_.push_back(timing);
    }

    // A GPU timestamp as std::chrono::steady_clock time (which counts
    // QueryPerformanceCounter from zero), from a calibration of its queue's
    // GPU and CPU clocks.
    static std::chrono::steady_clock::time_point SteadyTime(UINT64 stamp, UINT64 calibration_gpu, UINT64 calibration_cpu,
                                                            UINT64 gpu_frequency, LONGLONG cpu_frequency) {
        const double seconds = static_cast<double>(calibration_cpu) / static_cast<double>(cpu_frequency) +
                               (static_cast<double>(stamp) - static_cast<double>(calibration_gpu)) /
                                   static_cast<double>(gpu_frequency);
        return std::chrono::steady_clock::time_point(
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(seconds)));
    }

    // A capture list's GPU timestamps (on the game's queue): at its start,
    // and at its end with both resolved for reading.
    void StampCapture(std::size_t slot_index, bool end) {
        if (!capture_query_heap_) {
            return;
        }
        const UINT first = static_cast<UINT>(slot_index * 2);
        capture_commands_->EndQuery(capture_query_heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, first + (end ? 1u : 0u));
        if (end) {
            capture_commands_->ResolveQueryData(capture_query_heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, first, 2,
                                                capture_readback_.Get(), first * sizeof(UINT64));
        }
    }

    // Once a capture has run: when it finished, kept by its fence value for
    // the analyses that waited for it (its slot is then free to reuse).
    void KeepCaptureEnd(CaptureSlot& slot, std::size_t slot_index) {
        ID3D12Fence* fence = CaptureFence();
        if (!slot.timed || !fence || fence->GetCompletedValue() < slot.fence_value) {
            return;
        }
        slot.timed = false;
        LARGE_INTEGER frequency{};
        if (!capture_readback_ || !capture_frequency_ || !capture_calibration_cpu_ || !QueryPerformanceFrequency(&frequency)) {
            return;
        }
        const D3D12_RANGE range{(slot_index * 2 + 1) * sizeof(UINT64), (slot_index * 2 + 2) * sizeof(UINT64)};
        void* mapped = nullptr;
        if (FAILED(capture_readback_->Map(0, &range, &mapped))) {
            return;
        }
        UINT64 stamp = 0;
        std::memcpy(&stamp, static_cast<const std::byte*>(mapped) + range.Begin, sizeof(stamp));
        const D3D12_RANGE written{0, 0};
        capture_readback_->Unmap(0, &written);
        capture_ends_[capture_end_next_++ % capture_ends_.size()] = {
            slot.fence_value, SteadyTime(stamp, capture_calibration_gpu_, capture_calibration_cpu_, capture_frequency_,
                                         frequency.QuadPart)};
    }

    // When the capture that signalled `value` finished on the GPU ({}: not
    // known).
    std::chrono::steady_clock::time_point CaptureEnd(UINT64 value) {
        if (!value) {
            return {};
        }
        for (std::size_t i = 0; i < capture_slots_.size(); ++i) {
            if (capture_slots_[i].timed && capture_slots_[i].fence_value == value) {
                KeepCaptureEnd(capture_slots_[i], i);
            }
        }
        // An engine motion submission may be the newest capture an analysis
        // waited for (CaptureMotion).
        if (std::any_of(motion_slots_.begin(), motion_slots_.end(),
                        [&](const MotionSlot& slot) { return slot.timed && slot.fence_value == value; })) {
            ReadMotion();
        }
        for (const auto& [fence_value, time] : capture_ends_) {
            if (fence_value == value) {
                return time;
            }
        }
        return {};
    }

    bool Writable(std::uint64_t frame_id, bool exact) const override {
        const AnalyzedFrame* frame = FindAnalyzed(frame_id);
        return frame && ImagesKept(*frame, exact);
    }

    // Analyzed real frame `frame_id` while it is kept (null: never analyzed,
    // or dropped since).
    const AnalyzedFrame* FindAnalyzed(std::uint64_t frame_id) const {
        const auto found = std::find_if(analyzed_.begin(), analyzed_.end(),
                                        [&](const AnalyzedFrame& frame) { return frame.id == frame_id; });
        return found == analyzed_.end() ? nullptr : &*found;
    }

    // Whether every view's images `frame` is written from are still in the
    // history ring (`exact`: only its newest).
    bool ImagesKept(const AnalyzedFrame& frame, bool exact) const {
        if (views_.size() < frame.views.size()) {
            return false;
        }
        for (std::size_t i = 0; i < frame.views.size(); ++i) {
            const AnalyzedView& analyzed = frame.views[i];
            const ViewResources& view = views_[i];
            if (!analyzed.valid || view.slot_generation[analyzed.newest] != analyzed.newest_generation ||
                (analyzed.request.synthesize && !exact &&
                 view.slot_generation[analyzed.previous] != analyzed.previous_generation)) {
                return false;
            }
        }
        return true;
    }

    // The frame functions' checks (Writable's): analyzed real frame
    // `frame_id` with every view's images still kept (`exact`: only its
    // newest), or null with the reason.
    const AnalyzedFrame* KeptFrame(std::uint64_t frame_id, bool exact, std::string& error) const {
        const AnalyzedFrame* frame = FindAnalyzed(frame_id);
        if (!frame || views_.size() < frame->views.size()) {
            error = "the real frame is no longer kept";
            return nullptr;
        }
        if (!ImagesKept(*frame, exact)) {
            error = "the real frame's images were replaced by newer ones";
            return nullptr;
        }
        return frame;
    }

    // Sets one view's frame of `recording`'s plan up before anything is
    // recorded: drawn straight into its one output, or else written into
    // the view's own image first (created here).
    bool PrepareFrame(ViewRecording& recording, std::string& error) {
        recording.frames.assign(1, nullptr);
        for (const FrameGenDispatch& dispatch : recording.plan.dispatches) {
            if (Generates(dispatch)) recording.frames[0] = &dispatch;
        }
        recording.draw = recording.frames[0] && CanDraw(*recording.submission, recording.outputs, 1, false);
        return recording.draw || EnsureOutput(*recording.view, *recording.submission, error);
    }

    // Records one view's frame of `recording`'s plan (PrepareFrame set it
    // up) into its one output, from the analyzed frame's ring slots.
    bool RecordFrame(ViewRecording& recording, const AnalyzedView& analyzed, std::string& error) {
        ViewResources& view = *recording.view;
        view.bound_newest = analyzed.newest;
        view.bound_previous = analyzed.previous;
        const bool ok = RecordPlan(recording, false, error);
        view.bound_newest = view.bound_previous = -1;
        return ok;
    }

    // The frame functions' common body: `record` records a frame written
    // just in time on the synthesis list, into a synthesis slot's
    // descriptors; it is submitted on the present queue once real frame
    // `frame` was captured (its exact copy) or measured.
    template <typename Record>
    bool RecordSynthesis(const AnalyzedFrame& frame, bool exact, Record&& record, std::string& error) {
        const std::size_t slot_index = static_cast<std::size_t>(synthesis_count_ % kSynthesisSlots);
        SynthesisSlot& slot = synthesis_slots_[slot_index];
        if (!WaitFence(slot.fence_value, 100, slot.fence)) {
            error = "D3D12 Frame Generation synthesis did not finish in time";
            return false;
        }
        HRESULT hr = slot.allocator->Reset();
        if (SUCCEEDED(hr)) hr = synthesis_commands_->Reset(slot.allocator.Get(), nullptr);
        if (FAILED(hr)) {
            error = Failure("D3D12 Frame Generation synthesis reset", hr);
            return false;
        }
        // Recorded by the frame functions on the synthesis list, into the
        // synthesis slot's descriptors.
        std::swap(commands_, synthesis_commands_);
        std::swap(commands5_, synthesis_commands5_);
        const UINT saved_cursor = descriptor_cursor_;
        const UINT saved_end = descriptor_end_;
        descriptor_cursor_ = kSynthesisDescriptors + static_cast<UINT>(slot_index) * kDescriptorsPerSynthesis;
        descriptor_end_ = descriptor_cursor_ + kDescriptorsPerSynthesis;
        ID3D12DescriptorHeap* heaps[] = {heap_.Get()};
        commands_->SetDescriptorHeaps(1, heaps);
        commands_->SetComputeRootSignature(root_signature_.Get());
        profile_labels_.clear();
        if (profiling_) commands_->EndQuery(profile_heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
        bool ok = record(error);
        if (profiling_) {
            commands_->ResolveQueryData(profile_heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0,
                                        static_cast<UINT>(profile_labels_.size() + 1), profile_readback_.Get(), 0);
        }
        hr = commands_->Close();
        std::swap(commands_, synthesis_commands_);
        std::swap(commands5_, synthesis_commands5_);
        descriptor_cursor_ = saved_cursor;
        descriptor_end_ = saved_end;
        if (ok && FAILED(hr)) {
            error = Failure("D3D12 Frame Generation synthesis close", hr);
            ok = false;
        }
        if (!ok) {
            // Nothing was submitted: the recorded state transitions never ran.
            // Rebuild the views once earlier submissions are done with them.
            const bool idle = WaitIdle();
            for (std::size_t i = 0; i < frame.views.size() && i < views_.size(); ++i) {
                if (idle) {
                    RetireHardware(views_[i]);
                    views_[i] = {};
                } else {
                    views_[i].history_state = {};
                }
            }
            analyzed_.clear();
            return false;
        }
        // Its images were captured before the Execute that analyzed them,
        // which waited for that: newer captures (queued behind the game's
        // next frame on its queue) are not waited for.
        ID3D12Fence* done = nullptr;
        UINT64 done_value = 0;
        hr = exact ? SubmitPresent(synthesis_commands_.Get(), CaptureFence(), frame.capture_value, done, done_value)
                   : SubmitPresent(synthesis_commands_.Get(), fence_.Get(), frame.work_value, done, done_value);
        for (std::size_t i = 0; i < frame.views.size(); ++i) {
            MarkUsed(views_[i], done, done_value);
        }
        if (FAILED(hr)) {
            error = Failure("D3D12 Frame Generation synthesis signal", hr);
            signal_failed_ = true;
            return false;
        }
        slot.fence = done;
        slot.fence_value = done_value;
        for (std::size_t i = 0; i < frame.views.size(); ++i) {
            ViewResources& view = views_[i];
            auto& read = done == present_fence_.Get() ? view.slot_read_present : view.slot_read;
            if (done == fence_.Get() || done == present_fence_.Get()) {
                read[frame.views[i].newest] = read[frame.views[i].previous] = done_value;
            }
        }
        ++synthesis_count_;
        if (profiling_) {
            ReadProfile();
        }
        return true;
    }

    // Quad views: the periphery's filtered flow (and cover flow) inside each
    // eye's focus rectangle replaced by its focus view's (FG_PASS_FLOW_MERGE),
    // where both views measured motion this frame.
    bool RecordFlowMerge(const FrameGenComposition& composition, std::vector<ViewRecording>& recordings,
                         std::string& error) {
        for (const FrameGenComposeEye& eye : composition.eyes) {
            if (eye.peripheral >= recordings.size() || eye.focus >= recordings.size() || eye.peripheral == eye.focus ||
                !recordings[eye.peripheral].plan.synthesize || !recordings[eye.focus].plan.synthesize) {
                continue;
            }
            const FrameGenViewPlan& periphery_plan = recordings[eye.peripheral].plan;
            const FrameGenViewPlan& focus_plan = recordings[eye.focus].plan;
            ViewResources& periphery = *recordings[eye.peripheral].view;
            ViewResources& focus = *recordings[eye.focus].view;
            const std::uint8_t periphery_newest = periphery.history_state.Newest();
            const std::uint8_t focus_newest = focus.history_state.Newest();
            Texture& flow = periphery.filtered[periphery_newest];
            Texture& focus_flow = focus.filtered[focus_newest];
            // Cover flow is filtered for both views of an interpolated frame.
            const bool cover = periphery.filtered_cover[periphery_newest].resource &&
                               focus.filtered_cover[focus_newest].resource &&
                               std::any_of(periphery_plan.dispatches.begin(), periphery_plan.dispatches.end(),
                                           [](const FrameGenDispatch& dispatch) {
                                               return dispatch.outputs[0].kind == FrameGenTextureKind::kFilteredCover;
                                           });
            if (!flow.resource || !focus_flow.resource) {
                continue;
            }
            if (descriptor_cursor_ + kDescriptorsPerDispatch > descriptor_end_) {
                error = "D3D12 Frame Generation descriptor budget exhausted";
                return false;
            }
            const UINT base = descriptor_cursor_;
            descriptor_cursor_ += kDescriptorsPerDispatch;
            std::vector<D3D12_RESOURCE_BARRIER> barriers;
            Texture* inputs[2] = {&focus_flow, cover ? &focus.filtered_cover[focus_newest] : nullptr};
            Texture* outputs[2] = {&flow, cover ? &periphery.filtered_cover[periphery_newest] : nullptr};
            for (UINT i = 0; i < kShaderResources; ++i) {
                Texture* texture = i < 2 ? inputs[i] : nullptr;
                ShaderResource(texture, base, i);
                if (texture) Require(barriers, *texture, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            }
            for (UINT i = 0; i < 2; ++i) {
                UnorderedAccess(outputs[i], base + kFirstUav + i);
                if (outputs[i]) Require(barriers, *outputs[i], D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            }
            Flush(barriers);
            const FrameGenExtent cells = periphery_plan.levels[periphery_plan.final_flow_level];
            const FrameGenExtent focus_cells = focus_plan.levels[focus_plan.final_flow_level];
            FrameGenConstants constants{};
            constants.fov_current = eye.focus_rect;
            constants.size = {cells.width, cells.height, focus_cells.width, focus_cells.height};
            constants.options = {cover ? 1u : 0u, 0u, 0u, 0u};
            commands_->SetPipelineState(flow_merge_pipeline_.Get());
            commands_->SetComputeRoot32BitConstants(0, sizeof(FrameGenConstants) / 4, &constants, 0);
            commands_->SetComputeRootDescriptorTable(1, Gpu(base));
            commands_->SetComputeRootDescriptorTable(2, Gpu(base + kFirstUav));
            commands_->Dispatch((cells.width + kFrameGenThreadGroupSize - 1) / kFrameGenThreadGroupSize,
                                (cells.height + kFrameGenThreadGroupSize - 1) / kFrameGenThreadGroupSize, 1);
            Stamp(kProfileFlowMerge);
        }
        return true;
    }

    // A view's compose target: the view's size, stored as an output of its
    // kind is (sRGB, UNORM or half float), so a frame drawn or copied into
    // it reads back as written.
    bool EnsureComposeTarget(ViewResources& view, const FrameGenViewRequest& request, std::string& error) {
        const DXGI_FORMAT view_format = static_cast<DXGI_FORMAT>(OutputFormats(request.output).front());
        if (view.compose_target.resource) {
            const D3D12_RESOURCE_DESC desc = view.compose_target.resource->GetDesc();
            if (desc.Width == request.extent.width && desc.Height == request.extent.height &&
                view.compose_target.view_format == view_format) {
                return true;
            }
            if (!WaitIdle()) {
                error = "D3D12 Frame Generation GPU work did not finish before reallocation";
                return false;
            }
        }
        const DXGI_FORMAT format = view_format == DXGI_FORMAT_R16G16B16A16_FLOAT ? view_format : DXGI_FORMAT_R8G8B8A8_TYPELESS;
        return Create(view.compose_target, request.extent, format, view_format, false, error, false, true);
    }

    // The graphics pipeline composing an eye into a render target of this
    // format; null if it cannot be created. Under the caller's GPU lock.
    ID3D12PipelineState* ComposePipeline(DXGI_FORMAT format) {
        const auto found = compose_pipelines_.find(format);
        if (found != compose_pipelines_.end()) {
            return found->second.Get();
        }
        const framegen_shaders::Blob& pixel = framegen_shaders::kDxbcDraw[framegen_shaders::kComposeDrawShader];
        ComPtr<ID3D12PipelineState> pipeline;
        if (pixel.data && pixel.size) {
            pipeline = CreateDrawPipeline(pixel, &format, 1);
        }
        compose_pipelines_[format] = pipeline;
        return pipeline.Get();
    }

    // Whether an eye can be composed into `image` (an acquired output): a
    // tracked, single-sampled render target of a typed format, the composed
    // image's size, with a compose pipeline for its format. `output`
    // receives the image's resource.
    bool ComposeOutput(const FrameGenImage& image, FrameGenExtent extent, ID3D12Resource*& output, std::string& error) {
        const auto output_it = images_.find(image.swapchain);
        if (output_it == images_.end() || image.image_index >= output_it->second.size()) {
            error = "Frame Generation output image is not tracked";
            return false;
        }
        output = output_it->second[image.image_index];
        const D3D12_RESOURCE_DESC desc = output->GetDesc();
        const DXGI_FORMAT format = static_cast<DXGI_FORMAT>(image.format);
        if (!(desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) || desc.SampleDesc.Count != 1 ||
            image.array_index >= desc.DepthOrArraySize || image.rect.offset.x != 0 || image.rect.offset.y != 0 ||
            static_cast<std::uint32_t>(image.rect.extent.width) != extent.width ||
            static_cast<std::uint32_t>(image.rect.extent.height) != extent.height || format == DXGI_FORMAT_UNKNOWN ||
            format == DXGI_FORMAT_R8G8B8A8_TYPELESS || format == DXGI_FORMAT_B8G8R8A8_TYPELESS ||
            format == DXGI_FORMAT_R16G16B16A16_TYPELESS) {
            error = "a composed eye's output image cannot be drawn into";
            return false;
        }
        if (!ComposePipeline(format)) {
            error = "D3D12 Frame Generation compose pipeline unavailable";
            return false;
        }
        return true;
    }

    // Composes one eye into `image`, `output` (ComposeOutput accepted it),
    // from its peripheral and focus views' images (FG_PASS_COMPOSE_DRAW).
    bool RecordCompose(Texture& peripheral, Texture& focus, const FrameGenImage& image, ID3D12Resource* output,
                       const FrameGenConstants& constants, FrameGenExtent extent, std::string& error) {
        const D3D12_RESOURCE_DESC desc = output->GetDesc();
        const DXGI_FORMAT format = static_cast<DXGI_FORMAT>(image.format);
        ID3D12PipelineState* pipeline = ComposePipeline(format);
        if (!pipeline) {
            error = "D3D12 Frame Generation compose pipeline unavailable";
            return false;
        }
        if (descriptor_cursor_ + kDescriptorsPerDispatch > descriptor_end_) {
            error = "D3D12 Frame Generation descriptor budget exhausted";
            return false;
        }
        const UINT base = descriptor_cursor_;
        descriptor_cursor_ += kDescriptorsPerDispatch;
        std::vector<D3D12_RESOURCE_BARRIER> barriers;
        Texture* inputs[2] = {&peripheral, &focus};
        for (UINT i = 0; i < kShaderResources; ++i) {
            Texture* texture = i < 2 ? inputs[i] : nullptr;
            ShaderResource(texture, base, i);
            if (texture) Require(barriers, *texture, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        }
        UnorderedAccess(nullptr, base + kFirstUav);
        UnorderedAccess(nullptr, base + kFirstUav + 1);
        Flush(barriers);
        // Output images are in RENDER_TARGET while acquired.
        D3D12_RENDER_TARGET_VIEW_DESC rtv{};
        rtv.Format = format;
        if (desc.DepthOrArraySize > 1) {
            rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
            rtv.Texture2DArray.FirstArraySlice = image.array_index;
            rtv.Texture2DArray.ArraySize = 1;
        } else {
            rtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        }
        D3D12_CPU_DESCRIPTOR_HANDLE target = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
        target.ptr += static_cast<SIZE_T>(rtv_cursor_++ % kRenderTargetViews) * rtv_size_;
        device_->CreateRenderTargetView(output, &rtv, target);
        const D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(extent.width), static_cast<float>(extent.height),
                                      0.0f, 1.0f};
        const D3D12_RECT scissor{0, 0, static_cast<LONG>(extent.width), static_cast<LONG>(extent.height)};
        commands_->SetGraphicsRootSignature(root_signature_.Get());
        commands_->SetPipelineState(pipeline);
        commands_->SetGraphicsRoot32BitConstants(0, sizeof(FrameGenConstants) / 4, &constants, 0);
        commands_->SetGraphicsRootDescriptorTable(1, Gpu(base));
        commands_->SetGraphicsRootDescriptorTable(2, Gpu(base + kFirstUav));
        commands_->RSSetViewports(1, &viewport);
        commands_->RSSetScissorRects(1, &scissor);
        commands_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        commands_->OMSetRenderTargets(1, &target, FALSE, nullptr);
        commands_->DrawInstanced(3, 1, 0, 0);
        Stamp(kProfileCompose);
        return true;
    }

    void SetCopyAfterRelease(bool on) override { copy_after_release_ = on; }

    // A view whose image was not captured when the game released it cannot
    // be generated from: the image belongs to the runtime after its release
    // (it may be reading it, in a state of its choosing). The views are set
    // up for the next releases to be captured, and nothing is recorded.
    bool Uncaptured(const std::vector<FrameGenViewSubmission>& views, const FrameGenTuning& tuning,
                    FrameGenExecuteResult& result, std::string& error) {
        if (copy_after_release_) {
            return false;
        }
        if (views_.size() < views.size()) {
            views_.resize(views.size());
        }
        // A capture made for another ring size (batch or just in time, a
        // canvas or quad views) is into textures EnsureView replaces.
        const std::uint8_t ring = FrameGenHistoryRing(tuning);
        bool captured = true;
        for (std::size_t i = 0; i < views.size(); ++i) {
            captured = captured && views_[i].capture.Matches(views[i].source) && views_[i].history_state.Ring() == ring;
        }
        if (captured) {
            return false;
        }
        for (std::size_t i = 0; i < views.size(); ++i) {
            const auto it = images_.find(views[i].source.swapchain);
            if (it == images_.end() || views[i].source.image_index >= it->second.size()) {
                error = "Frame Generation image is not tracked";
                return true;
            }
            if (!EnsureView(views_[i], views[i], it->second[views[i].source.image_index], tuning, error)) {
                return true;
            }
            // Engine motion vectors captured since were paired with an
            // earlier color capture (a frame never analysed): no later
            // frame's either.
            views_[i].capture.valid = false;
            views_[i].engine_ready = false;
        }
        result.uncaptured = true;
        error = "a view's image was not captured when the game released it";
        return true;
    }

    bool DumpNextExecute(const std::string& directory) override {
        dump_directory_ = directory;
        // The frames of one capture share a parent: the game's raw depth of
        // each view's next capture goes there, armed by every dumped frame
        // (the first dumped frame's depth was captured before this).
        raw_depth_root_ = std::filesystem::path(directory).parent_path().string();
        raw_depth_left_ = static_cast<std::uint32_t>(std::max<std::size_t>(views_.size(), 2));
        return true;
    }

    void ResetHistory() override {
        for (auto& view : views_) {
            view.history_state.Invalidate();
        }
        analyzed_.clear();
    }

    void Reset() override {
        if (!stalled_ && !WaitIdle()) {
            // Teardown with a stalled GPU: everything the recorded work may
            // still use is leaked rather than freed under it.
            LeakAll();
        }
        StopHardwareWatchdog();
        for (auto& view : views_) {
            RetireHardware(view);
        }
        RetireAtlases();
        hardware_loaded_ = false;
        views_.clear();
        analyzed_.clear();
        images_.clear();
        if (!stalled_) {
            motion_views_.clear();
        }
        for (MotionSlot& slot : motion_slots_) {
            slot.pending.clear();
        }
        motion_census_.clear();
        if (event_ && !stalled_) {
            CloseHandle(event_);
        }
        if (capture_wait_event_ && !stalled_) {
            CloseHandle(capture_wait_event_);
        }
        event_ = nullptr;
        capture_wait_event_ = nullptr;
    }

    // Detaches every object submitted work can reference: the views' images,
    // command allocators and lists, descriptor heaps, pipelines, query and
    // readback buffers, the fence, and the event the fence may still signal
    // (closing it would let a reused handle be signalled). From here
    // WaitIdle reports the GPU as busy, so callers keep what it uses too.
    void LeakAll() {
        stalled_ = true;
        hardware_hung_ = true; // RetireHardware keeps the optical flow images as well
        for (auto& view : views_) {
            ForEachTexture(view, [](Texture& texture) { texture.resource.Detach(); });
        }
        root_signature_.Detach();
        for (auto& pipeline : pipelines_) pipeline.Detach();
        for (auto& [key, pipeline] : draw_pipelines_) pipeline.Detach();
        for (auto& [format, pipeline] : compose_pipelines_) pipeline.Detach();
        flow_merge_pipeline_.Detach();
        heap_.Detach();
        rtv_heap_.Detach();
        clear_heap_.Detach();
        commands_.Detach();
        capture_commands_.Detach();
        synthesis_commands_.Detach();
        for (auto& slot : synthesis_slots_) slot.allocator.Detach();
        if (separate_) {
            work_.Detach();
            capture_fence_.Detach();
        }
        present_fence_.Detach();
        for (auto& slot : slots_) {
            slot.allocator.Detach();
            slot.after_hardware.Detach();
            slot.check_readback.Detach();
            slot.engine_readback.Detach();
        }
        for (auto& slot : capture_slots_) slot.allocator.Detach();
        for (auto& slot : motion_slots_) {
            slot.allocator.Detach();
            slot.readback.Detach();
        }
        for (auto& outputs : motion_views_) {
            outputs.census.resource.Detach();
            outputs.samples.resource.Detach();
        }
        check_scores_.resource.Detach();
        query_heap_.Detach();
        readback_.Detach();
        capture_query_heap_.Detach();
        capture_readback_.Detach();
        profile_heap_.Detach();
        profile_readback_.Detach();
        fence_.Detach();
    }

    double LastGpuMilliseconds() const override { return last_gpu_ms_; }
    std::optional<FrameGenGpuWindow> LastGpuWindow() const override { return last_window_; }
    std::uint64_t WorkingMemoryBytes() const override {
        std::uint64_t bytes = 0;
        auto add = [&](const Texture& texture) {
            if (!texture.resource) return;
            const D3D12_RESOURCE_DESC desc = texture.resource->GetDesc();
            bytes += device_->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
        };
        for (const ViewResources& view : views_) {
            ForEachTexture(view, add);
        }
#ifdef DEPTHXR_NVOF
        // Atlas matching owns images in addition to each view's packed
        // input/output. Cached atlases still consume video memory while a
        // different quality or view layout is active.
        const auto add_atlas = [&](const HardwareAtlas& atlas) {
            add(atlas.newest);
            add(atlas.previous);
            add(atlas.flow);
        };
        add_atlas(atlas_);
        for (const HardwareAtlas& atlas : atlas_cache_) add_atlas(atlas);
#endif
        add(check_scores_);
        for (const MotionView& outputs : motion_views_) {
            add(outputs.census);
            add(outputs.samples);
        }
        return bytes;
    }

    // Every image of a view, one list for LeakAll (which must keep all that
    // submitted work may use), WorkingMemoryBytes and TrimViews: the view's
    // own, then those registered with its optical flow session.
    template <typename View, typename Visit>
    static void ForEachTexture(View& view, Visit&& visit) {
        ForEachOwnTexture(view, visit);
        for (auto* texture : {&view.hardware_newest, &view.hardware_previous, &view.hardware_flow, &view.raw[0],
                              &view.raw[1], &view.raw_flow}) {
            visit(*texture);
        }
    }

    // A view's own images (TrimViews frees them).
    template <typename View, typename Visit>
    static void ForEachOwnTexture(View& view, Visit&& visit) {
        for (auto* group : {&view.history, &view.filtered, &view.filtered_cover, &view.depth, &view.trust}) {
            for (auto& texture : *group) visit(texture);
        }
        for (auto* group : {&view.luma_newest, &view.luma_previous, &view.flow}) {
            for (auto& texture : *group) visit(texture);
        }
        for (auto& texture : view.engine_raw) visit(texture);
        for (auto& texture : view.engine_census) visit(texture);
        for (auto* texture : {&view.output, &view.compose_target, &view.shift_keys, &view.shift_map, &view.motion_map,
                              &view.cross_keys,
                              &view.depth_stale,
                              &view.world_partials, &view.world_params, &view.shading_rate, &view.trust_tiles,
                              &view.trust_votes, &view.depth_scales, &view.depth_scale_raw, &view.depth_scale_votes,
                              &view.engine_partials, &view.engine_params, &view.engine_evidence}) {
            visit(*texture);
        }
    }

    std::optional<FrameGenMemoryBudget> MemoryBudget() const override {
        DXGI_QUERY_VIDEO_MEMORY_INFO info{};
        if (!memory_adapter_ || FAILED(memory_adapter_->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) {
            return std::nullopt;
        }
        return FrameGenMemoryBudget{info.CurrentUsage, info.Budget};
    }

  private:
    // The state a game's motion vector images are taken to be in around a
    // read (VECTORXR_FRAMEGEN_ENGINE_MV_STATE): render target, as colour
    // images are at their release (auto, unless they allow simultaneous
    // access: then, like none, no barrier and the read promotes them), or
    // common.
    enum class MotionState { kAuto, kRenderTarget, kCommon, kNone };
    std::optional<D3D12_RESOURCE_STATES> MotionHeldState(bool simultaneous) const {
        switch (motion_state_) {
        case MotionState::kRenderTarget:
            return D3D12_RESOURCE_STATE_RENDER_TARGET;
        case MotionState::kCommon:
            return D3D12_RESOURCE_STATE_COMMON;
        case MotionState::kNone:
            return std::nullopt;
        default:
            return simultaneous ? std::nullopt : std::optional<D3D12_RESOURCE_STATES>(D3D12_RESOURCE_STATE_RENDER_TARGET);
        }
    }

    // The format a motion vector image is read as: its own, or for a
    // typeless one the swapchain's; none for anything but float vectors.
    static DXGI_FORMAT MotionViewFormat(DXGI_FORMAT native, std::int64_t created) {
        DXGI_FORMAT format = native;
        switch (native) {
        case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        case DXGI_FORMAT_R16G16_TYPELESS:
        case DXGI_FORMAT_R32G32B32A32_TYPELESS:
        case DXGI_FORMAT_R32G32_TYPELESS:
            format = static_cast<DXGI_FORMAT>(created);
            break;
        default:
            break;
        }
        switch (format) {
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R16G16_FLOAT:
        case DXGI_FORMAT_R32G32B32A32_FLOAT:
        case DXGI_FORMAT_R32G32_FLOAT:
            return format;
        default:
            return DXGI_FORMAT_UNKNOWN;
        }
    }

    // Each motion vector image's resource and heap, once, for the log: what
    // the runtime made them as says how they may be read.
    void DescribeMotionImage(ID3D12Resource* resource, const D3D12_RESOURCE_DESC& desc, XrSwapchain swapchain,
                             std::uint32_t index) {
        if (!motion_described_.insert(resource).second || motion_notes_.size() >= kMotionNotes) {
            return;
        }
        D3D12_HEAP_PROPERTIES heap{};
        D3D12_HEAP_FLAGS heap_flags{};
        const bool placed = SUCCEEDED(resource->GetHeapProperties(&heap, &heap_flags));
        char text[512];
        std::snprintf(text, sizeof(text),
                      "motion vector image %p[%u]: %llux%u, %u slices, %u mips, format %d, %u samples, flags 0x%x, "
                      "layout %d; heap %s type %d, flags 0x%x; read as %s",
                      static_cast<void*>(swapchain), index, static_cast<unsigned long long>(desc.Width), desc.Height,
                      desc.DepthOrArraySize, desc.MipLevels, static_cast<int>(desc.Format), desc.SampleDesc.Count,
                      static_cast<unsigned>(desc.Flags), static_cast<int>(desc.Layout), placed ? "" : "(unknown)",
                      placed ? static_cast<int>(heap.Type) : -1, placed ? static_cast<unsigned>(heap_flags) : 0u,
                      motion_state_ == MotionState::kRenderTarget ? "render target"
                      : motion_state_ == MotionState::kCommon     ? "common"
                      : motion_state_ == MotionState::kNone       ? "promoted (no barrier)"
                      : (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS) != 0
                          ? "promoted (simultaneous access)"
                          : "render target (auto)");
        motion_notes_.push_back(text);
    }

    // An engine motion list's GPU timestamps, after the captures'.
    void StampMotion(std::size_t slot_index, bool end) {
        if (!capture_query_heap_) {
            return;
        }
        const UINT first = static_cast<UINT>((kCaptureSlots + slot_index) * 2);
        capture_commands_->EndQuery(capture_query_heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, first + (end ? 1u : 0u));
        if (end) {
            capture_commands_->ResolveQueryData(capture_query_heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, first, 2,
                                                capture_readback_.Get(), first * sizeof(UINT64));
        }
    }

    // The first descriptor of a motion slot's block (CaptureMotion: the eyes'
    // censuses, then the resample's dispatches).
    static UINT MotionBlock(std::size_t slot_index, std::size_t block) {
        return kMotionDescriptors + static_cast<UINT>(slot_index * kMotionBlocks + block) * kDescriptorsPerDispatch;
    }

    // A motion vector image (all its slices) as the shaders read it; none
    // still describes a texture array.
    void MotionImageView(ID3D12Resource* resource, DXGI_FORMAT format, UINT array_size, UINT descriptor) {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = resource ? format : DXGI_FORMAT_R16G16B16A16_FLOAT;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2DArray.MipLevels = 1;
        srv.Texture2DArray.ArraySize = resource ? array_size : 1;
        device_->CreateShaderResourceView(resource, &srv, Cpu(descriptor));
    }

    // A view's raw engine motion field the latest capture wrote, and the one
    // before (kEngineRaw, kEngineRawPrevious).
    std::uint8_t EngineNewest(const ViewResources& view) const {
        return static_cast<std::uint8_t>((view.engine_next + engine_slots_ - 1u) % engine_slots_);
    }
    std::uint8_t EnginePrevious(const ViewResources& view) const {
        return static_cast<std::uint8_t>((view.engine_next + engine_slots_ - 2u) % engine_slots_);
    }

    // The device's removed reason (S_OK while it works). Removed within 2 s
    // of an engine motion submission, the game's next sessions read none
    // (FrameGenEngineRemovedPath; the owner deletes the file to read them
    // again): the research reads images a game may not expect to be read.
    HRESULT DeviceRemovedReason() {
        const HRESULT hr = device_->GetDeviceRemovedReason();
        if (FAILED(hr) && !motion_removed_noted_ && motion_submitted_at_ &&
            std::chrono::steady_clock::now() - *motion_submitted_at_ <= std::chrono::seconds(2)) {
            motion_removed_noted_ = true;
            const std::filesystem::path path = FrameGenEngineRemovedPath();
            if (!path.empty()) {
                std::error_code ignored;
                std::filesystem::create_directories(path.parent_path(), ignored);
                std::ofstream(path) << "The GPU device was removed (hr=" << static_cast<long>(hr)
                                    << ") within 2 s of VectorXR Frame Generation reading this game's motion vectors.\n"
                                       "While this file is here, its sessions read none. Delete it to read them again.\n";
            }
            if (motion_notes_.size() < kMotionNotes) {
                motion_notes_.push_back("the device was removed within 2 s of reading the game's motion vectors: "
                                        "later sessions of this game read none until " +
                                        PathToUtf8(path) + " is deleted");
            }
        }
        return hr;
    }

    // Submits an engine motion list on the game's queue: as SubmitCapture
    // (analyses wait for it), without waiting for a ring slot, as it writes
    // none. The raw fields it writes (`written` views' next ones) the
    // analysis of the frame that wrote them last (two captures ago, Execute's
    // engine_read) may still read on the work queue: as SubmitCapture's ring
    // slots, the game's queue waits for that only when the CPU sees it
    // unfinished (rarely, that frame being two frames old), and each such
    // view is counted. The count is what the CPU saw, not a stall: the
    // game's queue reaches the wait after rendering the frame, by when such
    // an analysis is usually done.
    HRESULT SubmitMotion(ID3D12GraphicsCommandList* list, const std::vector<ViewResources*>& written, UINT64& value) {
        if (separate_) {
            UINT64 read = 0;
            for (const ViewResources* view : written) {
                const UINT64 field = view->engine_read[view->engine_next];
                if (field && fence_->GetCompletedValue() < field) {
                    read = std::max(read, field);
                    ++motion_waits_;
                }
            }
            if (read) {
                queue_->Wait(fence_.Get(), read);
            }
        }
        ID3D12CommandList* lists[] = {list};
        queue_->ExecuteCommandLists(1, lists);
        const HRESULT hr = separate_ ? queue_->Signal(capture_fence_.Get(), ++capture_value_)
                                     : queue_->Signal(fence_.Get(), ++fence_value_);
        value = separate_ ? capture_value_ : fence_value_;
        last_capture_value_ = value; // CaptureMark
        return hr;
    }

    // One row texture into `buffer` at `offset` (512-byte aligned), on `list`.
    void CopyToReadback(ID3D12GraphicsCommandList* list, Texture& texture, ID3D12Resource* buffer, UINT64 offset) {
        const D3D12_RESOURCE_DESC desc = texture.resource->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        device_->GetCopyableFootprints(&desc, 0, 1, offset, &footprint, nullptr, nullptr, nullptr);
        D3D12_TEXTURE_COPY_LOCATION destination{buffer, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        destination.PlacedFootprint = footprint;
        D3D12_TEXTURE_COPY_LOCATION source{texture.resource.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    }

    // Reads every engine motion submission that has run since the last
    // call (never waits): its censuses, in submission order, for
    // EngineStatus, and when it ran (CaptureEnd).
    void ReadMotion() {
        ID3D12Fence* fence = CaptureFence();
        // A removed device reports every fence value reached, and its
        // readbacks hold nothing: nothing is read.
        if (!fence || FAILED(device_->GetDeviceRemovedReason())) {
            return;
        }
        const UINT64 completed = fence->GetCompletedValue();
        std::vector<FrameGenEngineCensus> read;
        for (std::size_t i = 0; i < motion_slots_.size(); ++i) {
            MotionSlot& slot = motion_slots_[i];
            if (!slot.fence_value || completed < slot.fence_value || (slot.pending.empty() && !slot.timed)) {
                continue;
            }
            double gpu_ms = -1.0;
            if (slot.timed) {
                slot.timed = false;
                gpu_ms = KeepMotionEnd(slot, i);
            }
            if (slot.pending.empty()) {
                continue;
            }
            void* mapped = nullptr;
            if (!slot.readback || FAILED(slot.readback->Map(0, nullptr, &mapped))) {
                slot.pending.clear();
                continue;
            }
            for (std::size_t j = 0; j < slot.pending.size(); ++j) {
                const MotionSlot::Pending& pending = slot.pending[j];
                const auto* region = static_cast<const std::byte*>(mapped) + j * kMotionViewReadback;
                FrameGenEngineCensus census;
                census.sequence = pending.sequence;
                census.frame = pending.frame;
                census.view = pending.view;
                census.early = pending.early;
                census.images = pending.images;
                census.censused = pending.censused;
                census.color_index = pending.color_index;
                census.depth_index = pending.depth_index;
                census.handle = reinterpret_cast<std::uint64_t>(pending.swapchain);
                census.rect = {pending.rect.offset.x, pending.rect.offset.y, pending.rect.extent.width,
                               pending.rect.extent.height};
                census.gpu_ms = gpu_ms;
                std::array<std::uint32_t, 16 * kFrameGenEngineImages> texels{};
                std::memcpy(texels.data(), region, sizeof(texels));
                for (std::uint32_t k = 0; k < pending.images && k < kFrameGenEngineImages; ++k) {
                    if (((pending.censused >> k) & 1u) == 0) {
                        continue; // not read: its texels are an older census's
                    }
                    const std::uint32_t* t = texels.data() + 16 * k;
                    FrameGenEngineImageCensus& image = census.image[k];
                    image.xor_hash = t[0];
                    image.sum_hash = t[1];
                    image.zeros = t[2];
                    image.non_finite = t[3];
                    image.large = t[4];
                    std::memcpy(&image.sum_abs, &t[5], sizeof(float));
                    std::memcpy(&image.max_abs, &t[6], sizeof(float));
                    image.finite = t[7];
                    std::memcpy(image.sums.data(), &t[8], 4 * sizeof(float));
                    image.tag = t[12];
                    image.image = t[13];
                    image.rect_width = t[14];
                    image.rect_height = t[15];
                }
                if (pending.samples) {
                    census.samples.resize(16 * 16 * 4);
                    std::memcpy(census.samples.data(), region + kMotionSamplesOffset,
                                census.samples.size() * sizeof(std::array<float, 4>));
                }
                read.push_back(std::move(census));
            }
            const D3D12_RANGE written{0, 0};
            slot.readback->Unmap(0, &written);
            slot.pending.clear();
        }
        std::sort(read.begin(), read.end(),
                  [](const FrameGenEngineCensus& a, const FrameGenEngineCensus& b) { return a.sequence < b.sequence; });
        for (FrameGenEngineCensus& census : read) {
            motion_census_.push_back(std::move(census));
        }
        while (motion_census_.size() > kMotionCensusKept) {
            motion_census_.pop_front();
        }
    }

    // A finished engine motion submission's stamps: when it ended, kept as a
    // capture's (CaptureEnd), and how long it ran (milliseconds, or -1).
    double KeepMotionEnd(const MotionSlot& slot, std::size_t slot_index) {
        LARGE_INTEGER frequency{};
        if (!capture_readback_ || !capture_frequency_ || !QueryPerformanceFrequency(&frequency)) {
            return -1.0;
        }
        const std::size_t first = (kCaptureSlots + slot_index) * 2;
        const D3D12_RANGE range{first * sizeof(UINT64), (first + 2) * sizeof(UINT64)};
        void* mapped = nullptr;
        if (FAILED(capture_readback_->Map(0, &range, &mapped))) {
            return -1.0;
        }
        UINT64 stamps[2]{};
        std::memcpy(stamps, static_cast<const std::byte*>(mapped) + range.Begin, sizeof(stamps));
        const D3D12_RANGE written{0, 0};
        capture_readback_->Unmap(0, &written);
        if (capture_calibration_cpu_) {
            capture_ends_[capture_end_next_++ % capture_ends_.size()] = {
                slot.fence_value, SteadyTime(stamps[1], capture_calibration_gpu_, capture_calibration_cpu_,
                                             capture_frequency_, frequency.QuadPart)};
        }
        return stamps[1] > stamps[0] ? 1000.0 * static_cast<double>(stamps[1] - stamps[0]) /
                                           static_cast<double>(capture_frequency_)
                                     : 0.0;
    }

    // A research capture's copy of one motion vector image: its region, as
    // stored, into a readback buffer of its own.
    struct MotionDump {
        std::string name;
        std::string meta;
        ComPtr<ID3D12Resource> buffer;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT rows{0};
        UINT64 row_bytes{0};
    };

    void DumpMotionImage(ID3D12Resource* resource, const FrameGenMotionJob& job, std::uint32_t index, bool all,
                         std::vector<MotionDump>& dumps) {
        const D3D12_RESOURCE_DESC desc = resource->GetDesc();
        // A region the game named outside the image is not copied: a copy
        // box past the image is invalid (the census reads zeros there).
        const XrRect2Di& rect = job.vectors.rect;
        if (rect.offset.x < 0 || rect.offset.y < 0 || rect.extent.width <= 0 || rect.extent.height <= 0 ||
            static_cast<UINT64>(rect.offset.x) + static_cast<UINT64>(rect.extent.width) > desc.Width ||
            static_cast<UINT64>(rect.offset.y) + static_cast<UINT64>(rect.extent.height) > desc.Height) {
            return;
        }
        D3D12_RESOURCE_DESC region = desc;
        region.Width = static_cast<UINT64>(job.vectors.rect.extent.width);
        region.Height = static_cast<UINT>(job.vectors.rect.extent.height);
        region.DepthOrArraySize = 1;
        region.MipLevels = 1;
        MotionDump dump;
        UINT64 bytes = 0;
        device_->GetCopyableFootprints(&region, 0, 1, 0, &dump.footprint, &dump.rows, &dump.row_bytes, &bytes);
        if (!bytes || bytes == UINT64_MAX) {
            return;
        }
        D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_READBACK};
        D3D12_RESOURCE_DESC buffer{};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = bytes;
        buffer.Height = buffer.DepthOrArraySize = buffer.MipLevels = 1;
        buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST,
                                                    nullptr, IID_PPV_ARGS(&dump.buffer)))) {
            return;
        }
        D3D12_TEXTURE_COPY_LOCATION destination{dump.buffer.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        destination.PlacedFootprint = dump.footprint;
        D3D12_TEXTURE_COPY_LOCATION source{resource, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        source.SubresourceIndex = job.vectors.array_index * std::max<UINT>(desc.MipLevels, 1u);
        const D3D12_BOX box{static_cast<UINT>(job.vectors.rect.offset.x), static_cast<UINT>(job.vectors.rect.offset.y), 0,
                            static_cast<UINT>(job.vectors.rect.offset.x + job.vectors.rect.extent.width),
                            static_cast<UINT>(job.vectors.rect.offset.y + job.vectors.rect.extent.height), 1};
        capture_commands_->CopyTextureRegion(&destination, 0, 0, 0, &source, &box);
        const DXGI_FORMAT format = MotionViewFormat(desc.Format, job.vectors.format);
        const char* extension = format == DXGI_FORMAT_R16G16_FLOAT         ? "rg16f"
                                : format == DXGI_FORMAT_R32G32B32A32_FLOAT ? "rgba32f"
                                : format == DXGI_FORMAT_R32G32_FLOAT       ? "rg32f"
                                                                           : "rgba16f";
        const std::string stem = "view" + std::to_string(job.view) + "-mv-img" + std::to_string(index);
        dump.name = stem + "." + extension;
        char meta[512];
        std::snprintf(meta, sizeof(meta),
                      "frame %llu\nview %zu\nimage %u\nswapchain %p\nslice %u\nrect %d %d %d %d\nformat %d\nfile %s\n"
                      "chosen %u\nall %d\ncolor_index %u\ndepth_index %u\n",
                      static_cast<unsigned long long>(job.frame), job.view, index, static_cast<void*>(job.vectors.swapchain),
                      job.vectors.array_index, job.vectors.rect.offset.x, job.vectors.rect.offset.y,
                      job.vectors.rect.extent.width, job.vectors.rect.extent.height, static_cast<int>(format),
                      dump.name.c_str(), job.vectors.image_index, all ? 1 : 0, job.color_index,
                      job.depth_index);
        dump.meta = meta;
        dumps.push_back(std::move(dump));
    }

    // On a research capture's writer thread: whether `fence` reaches `value`
    // within `timeout` ms. An event the fence may still signal is never
    // closed.
    static bool WaitOnDumpThread(ID3D12Fence* fence, UINT64 value, DWORD timeout) {
        if (!fence) {
            return false;
        }
        if (fence->GetCompletedValue() >= value) {
            return true;
        }
        const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        const bool done = event && SUCCEEDED(fence->SetEventOnCompletion(value, event)) &&
                          WaitForSingleObject(event, timeout) == WAIT_OBJECT_0;
        if (done) CloseHandle(event);
        return done;
    }

    // Writes a research capture's motion vector images once the GPU has
    // copied them: a thread of its own waits for `value` and writes them
    // (the game's thread never waits for it). Buffers the GPU never finishes
    // with are kept, never freed under it.
    void WriteMotionDump(std::vector<MotionDump> dumps, UINT64 value, const std::string& directory) {
        if (dumps.empty()) {
            return;
        }
        std::thread([dumps = std::move(dumps), fence = ComPtr<ID3D12Fence>(CaptureFence()), value, directory,
                     timeout = static_cast<DWORD>(10000 * wait_scale_)]() mutable {
            if (!WaitOnDumpThread(fence.Get(), value, timeout)) {
                for (MotionDump& dump : dumps) dump.buffer.Detach();
                return;
            }
            for (MotionDump& dump : dumps) {
                const std::string stem = directory + "\\" + dump.name.substr(0, dump.name.find('.'));
                std::ofstream(stem + ".txt") << dump.meta;
                void* mapped = nullptr;
                if (FAILED(dump.buffer->Map(0, nullptr, &mapped))) {
                    continue;
                }
                std::ofstream file(directory + "\\" + dump.name, std::ios::binary);
                for (UINT row = 0; row < dump.rows && file; ++row) {
                    file.write(static_cast<const char*>(mapped) + dump.footprint.Offset +
                                   row * dump.footprint.Footprint.RowPitch,
                               static_cast<std::streamsize>(dump.row_bytes));
                }
                dump.buffer->Unmap(0, nullptr);
            }
        }).detach();
    }

    // The debug layer's messages about resource states since the last read
    // (VECTORXR_FRAMEGEN_ENGINE_MV_DEBUG=1, only where the game runs with
    // it): its barrier checks are the only way to see the state a game left
    // its motion vector images in.
    void ReadDebugMessages() {
        if (!info_queue_) {
            return;
        }
        const UINT64 stored = info_queue_->GetNumStoredMessages();
        for (; motion_messages_read_ < stored && motion_notes_.size() < kMotionNotes; ++motion_messages_read_) {
            SIZE_T size = 0;
            if (FAILED(info_queue_->GetMessage(motion_messages_read_, nullptr, &size)) || size == 0) {
                continue;
            }
            std::vector<char> storage(size);
            auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
            if (FAILED(info_queue_->GetMessage(motion_messages_read_, message, &size)) || !message->pDescription) {
                continue;
            }
            const std::string description(message->pDescription, message->DescriptionByteLength ? message->DescriptionByteLength - 1 : 0);
            if (message->Severity <= D3D12_MESSAGE_SEVERITY_WARNING &&
                (description.find("state") != std::string::npos || description.find("State") != std::string::npos)) {
                motion_notes_.push_back("debug layer (message " + std::to_string(static_cast<int>(message->ID)) + "): " +
                                        description);
            }
        }
    }

    // A texture copied into a readback buffer for a research dump.
    struct DumpCopy {
        std::string name;
        ComPtr<ID3D12Resource> buffer;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT rows{0};
        UINT64 row_bytes{0};
        DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN};
    };

    // Copies every view's newest image and depth into readback buffers, and
    // the game's motion vectors resampled for the frame (kEngineCapture) as
    // view<i>-engine-raw.rg16f (.rgba16f with the research's b and a): only
    // the frame's own (ViewRecording::engine), which the capture into that
    // field waits for this work to read (Execute's engine_read).
    bool RecordDump(const std::vector<FrameGenViewSubmission>& views, const std::vector<ViewRecording>& recordings,
                    std::vector<DumpCopy>& dump, std::string& error) {
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
                !copy(view.depth[view.history_state.Newest()], "view" + std::to_string(i) + "-depth.f32")) {
                return false;
            }
            if (i < recordings.size() && recordings[i].engine &&
                !copy(view.engine_raw[EngineNewest(view)], "view" + std::to_string(i) + "-engine-raw." +
                                                               (engine_format_ == DXGI_FORMAT_R16G16_FLOAT ? "rg16f" : "rgba16f"))) {
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
        // What each view's depth was read from and how it was decoded, apart
        // from frame.txt (the replay reads that): which raw depth dump
        // (depth-raw-<capture>-view<i> next to the frames) it matches.
        std::ofstream depth_meta(dump_directory_ + "\\depth.txt");
        depth_meta.precision(9);
        depth_meta << "reading " << ToString(validator_.Reading()) << "\nswapped " << validator_.Swapped() << "\nscale "
                   << validator_.Scale() << "\nprobe_step " << validator_.ProbeStep() << "\ngeneration "
                   << validator_.Generation() << "\nempty_view "
                   << (eyes_.Empty() ? static_cast<int>(*eyes_.Empty()) : -1) << "\ncheck_source " << eyes_.Source()
                   << '\n';
        for (std::size_t i = 0; i < views.size() && i < views_.size(); ++i) {
            const ViewResources& view = views_[i];
            const FrameGenDepthEncoding& e = view.depth_encoding;
            const FrameGenImage& source = view.depth_source;
            depth_meta << "view " << i << " requested " << views[i].request.depth << " used "
                       << (i < result.depth.size() && result.depth[i]) << " capture "
                       << view.depth_capture << " generation " << view.depth_generation << " swapchain "
                       << source.swapchain << " image " << source.image_index << " slice " << source.array_index
                       << " rect " << source.rect.offset.x << ' ' << source.rect.offset.y << ' '
                       << source.rect.extent.width << ' ' << source.rect.extent.height << " min_depth " << e.min_depth
                       << " max_depth " << e.max_depth << " near_z " << e.near_z << " far_z " << e.far_z
                       << " crossed " << view.depth_crossed << '\n';
        }
        // Quad views: how each eye is composed from the views.
        if (tuning.composition) {
            const FrameGenComposition& composition = *tuning.composition;
            meta << "composition " << composition.output.width << ' ' << composition.output.height << " eyes "
                 << composition.eyes.size() << '\n';
            for (const FrameGenComposeEye& eye : composition.eyes) {
                meta << "eye " << eye.peripheral << ' ' << eye.focus << " rect " << eye.focus_rect[0] << ' '
                     << eye.focus_rect[1] << ' ' << eye.focus_rect[2] << ' ' << eye.focus_rect[3] << " feather "
                     << eye.feather[0] << ' ' << eye.feather[1] << " sharpen " << eye.sharpen << " texel "
                     << eye.focus_texel[0] << ' ' << eye.focus_texel[1] << " pixel " << eye.output_pixel_in_focus[0]
                     << ' ' << eye.output_pixel_in_focus[1] << '\n';
            }
        }
    }

    // Every wait shares one auto-reset event, so an earlier wait that timed
    // out can wake a later one early: wake, then re-check the fence itself.
    bool WaitFence(UINT64 value, DWORD timeout_ms, ID3D12Fence* fence = nullptr) {
        if (!fence) {
            fence = fence_.Get();
        }
        if (!fence || !value || fence->GetCompletedValue() >= value) {
            return true;
        }
        if (signal_failed_ || !event_ || FAILED(fence->SetEventOnCompletion(value, event_))) {
            return false;
        }
        const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(timeout_ms) * wait_scale_;
        while (fence->GetCompletedValue() < value) {
            const ULONGLONG now = GetTickCount64();
            if (now >= deadline) {
                return false;
            }
            WaitForSingleObject(event_, static_cast<DWORD>(std::min<ULONGLONG>(deadline - now, 10)));
        }
        return true;
    }

    // What captures signal: the game's queue has a fence of its own while
    // Frame Generation works on its own queue.
    ID3D12Fence* CaptureFence() const { return separate_ ? capture_fence_.Get() : fence_.Get(); }

    // Submits a capture list on the game's queue, once the work queue has
    // finished reading the ring slot it writes (the view's newest), and
    // returns the value its fence reaches when done in `value`.
    HRESULT SubmitCapture(ID3D12GraphicsCommandList* list, ViewResources& view, UINT64& value) {
        if (separate_) {
            const UINT64 read = view.slot_read[view.history_state.Newest()];
            if (read && fence_->GetCompletedValue() < read) {
                queue_->Wait(fence_.Get(), read);
            }
        }
        if (present_fence_) {
            const UINT64 read = view.slot_read_present[view.history_state.Newest()];
            if (read && present_fence_->GetCompletedValue() < read) {
                queue_->Wait(present_fence_.Get(), read);
            }
        }
        ID3D12CommandList* lists[] = {list};
        queue_->ExecuteCommandLists(1, lists);
        const HRESULT hr = separate_ ? queue_->Signal(capture_fence_.Get(), ++capture_value_)
                                     : queue_->Signal(fence_.Get(), ++fence_value_);
        value = separate_ ? capture_value_ : fence_value_;
        last_capture_value_ = value; // CaptureMark
        MarkUsed(view, CaptureFence(), value);
        return hr;
    }

    // `view`'s images are used by a submission that signals `fence` `value`.
    void MarkUsed(ViewResources& view, ID3D12Fence* fence, UINT64 value) {
        UINT64& used = fence == fence_.Get()           ? view.used_work
                       : fence == present_fence_.Get() ? view.used_present
                                                       : view.used_capture;
        used = std::max(used, value);
    }

    // Whether every submission that used `view`'s images has finished (the
    // optical flow hardware's included). Never waits.
    bool Unused(const ViewResources& view) const {
        ID3D12Fence* capture = CaptureFence();
        return !stalled_ && fence_ && fence_->GetCompletedValue() >= view.used_work &&
               (!view.used_capture || (capture && capture->GetCompletedValue() >= view.used_capture)) &&
               (!view.used_present || (present_fence_ && present_fence_->GetCompletedValue() >= view.used_present)) &&
               (!view.hardware_fence || view.hardware_fence->GetCompletedValue() >= view.hardware_fence_value);
    }

    // Views the frames no longer have (quad views' focus views once a
    // canvas replaced them): their images are freed as soon as no
    // submission uses them, without waiting, and frames analyzed with them
    // are forgotten. Their optical flow sessions stay for when they come
    // back: the driver's images cannot be freed (see RetireHardware).
    void TrimViews(std::size_t count) {
        bool trimmed = false;
        for (std::size_t i = count; i < views_.size(); ++i) {
            ViewResources& view = views_[i];
            bool images = false;
            ForEachOwnTexture(view, [&](const Texture& texture) { images = images || texture.resource; });
            if (!images || !Unused(view)) {
                continue; // nothing to free, or a later frame tries again
            }
            ForEachOwnTexture(view, [](Texture& texture) { texture = {}; });
            // Set up again (EnsureView) when a frame has it; frames analyzed
            // from its old images are not written.
            view.history_state = {};
            view.capture = {};
            for (auto& generation : view.slot_generation) ++generation;
            view.trust_stereo = view.depth_ready = view.depth_fresh = false;
            view.engine_pair = {};
            view.engine_read = {};
            view.engine_next = 0;
            view.engine_pair_next = view.engine_ready = false;
            view.engine_count = {};
            view.engine_captures = view.engine_used = 0;
            // Its optical flow session matches afresh (no hints, no raw
            // capture to use).
            view.hardware_fresh = true;
            view.raw_previous = false;
            view.raw_value = 0;
            view.raw_captured = view.raw_executed = 0;
            trimmed = true;
        }
        if (trimmed) {
            analyzed_.erase(std::remove_if(analyzed_.begin(), analyzed_.end(),
                                           [&](const AnalyzedFrame& frame) { return frame.views.size() > count; }),
                            analyzed_.end());
        }
    }

    // Submits Frame Generation's own work, after every capture so far
    // (`after_captures`).
    HRESULT SubmitWork(ID3D12GraphicsCommandList* list, bool after_captures = true) {
        if (after_captures && separate_ && capture_value_ > capture_waited_) {
            work_->Wait(capture_fence_.Get(), capture_value_);
            capture_waited_ = capture_value_;
        }
        ID3D12CommandList* lists[] = {list};
        work_->ExecuteCommandLists(1, lists);
        return work_->Signal(fence_.Get(), ++fence_value_);
    }

    // Output images written on the work queue: the game's queue (which the
    // runtime synchronizes with when they are released) waits for them. With
    // queue substitution the runtime synchronizes with the work queue itself.
    void HandOverOutputs() {
        if (present_.Get() != work_.Get() && fence_value_) {
            present_->Wait(fence_.Get(), fence_value_);
        }
    }

    // Submits a frame written just in time on the present queue, once its
    // real frame's measurement (`after`, on the work queue) is done; returns
    // the fence and value it reaches when done.
    HRESULT SubmitPresent(ID3D12GraphicsCommandList* list, ID3D12Fence* after_fence, UINT64 after, ID3D12Fence*& done,
                          UINT64& value) {
        if (present_.Get() == work_.Get()) {
            if (after_fence != fence_.Get() && after && after_fence->GetCompletedValue() < after) {
                work_->Wait(after_fence, after);
            }
            const HRESULT hr = SubmitWork(list, false);
            done = fence_.Get();
            value = fence_value_;
            return hr;
        }
        if (after && after_fence->GetCompletedValue() < after) {
            present_->Wait(after_fence, after);
        }
        ID3D12CommandList* lists[] = {list};
        present_->ExecuteCommandLists(1, lists);
        if (present_fence_) {
            done = present_fence_.Get();
            value = ++present_value_;
            return present_->Signal(present_fence_.Get(), present_value_);
        }
        // The game's queue itself (no queue substitution): the fence the
        // captures signal there.
        done = CaptureFence();
        value = ++capture_value_;
        return present_->Signal(done, value);
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
        // The submission's first and last stamps.
        const D3D12_RANGE range{slot_index * kTimingStamps * sizeof(UINT64), (slot_index + 1) * kTimingStamps * sizeof(UINT64)};
        void* mapped = nullptr;
        if (SUCCEEDED(readback_->Map(0, &range, &mapped))) {
            const auto* all = reinterpret_cast<const UINT64*>(static_cast<const std::byte*>(mapped) + range.Begin);
            const UINT64 stamps[2] = {all[0], all[kTimingStamps - 1]};
            if (stamps[1] >= stamps[0]) {
                last_gpu_ms_ = static_cast<double>(stamps[1] - stamps[0]) * 1000.0 / static_cast<double>(timestamp_frequency_);
            }
            LARGE_INTEGER frequency{};
            if (slot.timed_submission && stamps[1] >= stamps[0] && QueryPerformanceFrequency(&frequency)) {
                auto clock = [&](UINT64 stamp) {
                    return SteadyTime(stamp, slot.calibration_gpu, slot.calibration_cpu, timestamp_frequency_,
                                      frequency.QuadPart);
                };
                last_window_ = FrameGenGpuWindow{slot.timed_submission, clock(stamps[0]), clock(stamps[1])};
            }
            const D3D12_RANGE written{0, 0};
            readback_->Unmap(0, &written);
        }
    }

    // `render_target`: drawn into, created in RENDER_TARGET.
    bool Create(Texture& texture, FrameGenExtent extent, DXGI_FORMAT format, DXGI_FORMAT view_format, bool uav,
                std::string& error, bool common = false, bool render_target = false) {
        texture = {};
        if (allocation_failure_ && --allocation_failure_ == 0) {
            error = Failure("D3D12 Frame Generation texture", E_OUTOFMEMORY);
            return false;
        }
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
        if (render_target) desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        const D3D12_RESOURCE_STATES initial = common          ? D3D12_RESOURCE_STATE_COMMON
                                              : render_target ? D3D12_RESOURCE_STATE_RENDER_TARGET
                                              : uav           ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS
                                                              : D3D12_RESOURCE_STATE_COPY_DEST;
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
    // they are, for a ring of `ring` slots.
    bool Prepared(const FrameGenViewSubmission& submission, const ViewResources& view, std::uint8_t ring) const {
        const auto it = images_.find(submission.source.swapchain);
        if (it == images_.end() || submission.source.image_index >= it->second.size() || !view.depth[1].resource ||
            view.history_state.Ring() != ring) {
            return false;
        }
        return view.history_state.MatchesImages(submission.request.extent,
                                                it->second[submission.source.image_index]->GetDesc().Format,
                                                submission.request.output);
    }

    bool EnsureView(ViewResources& view, const FrameGenViewSubmission& submission, ID3D12Resource* source,
                    const FrameGenTuning& tuning, std::string& error) {
        // Uncaptured calls this before Execute's recording/error cleanup. A
        // partial allocation must never advertise its new images as ready,
        // nor keep a capture key that would let the next Execute skip
        // Uncaptured and copy the game's image after its release.
        struct Setup {
            ViewResources& view;
            bool complete{false};
            ~Setup() {
                if (!complete) {
                    view.history_state = {};
                    view.capture.valid = false;
                }
            }
        } setup{view};
        const D3D12_RESOURCE_DESC source_desc = source->GetDesc();
        const FrameGenExtent extent = submission.request.extent;
        const std::uint8_t final_level = FrameGenFinalFlowLevel(tuning);
        // Just-in-time synthesis keeps more real frames (see
        // FrameGenHistoryRing): a headset-resolution image per view each.
        const std::uint8_t ring = FrameGenHistoryRing(tuning);
        const unsigned change = view.history_state.Configure(extent, source_desc.Format, submission.request.output,
                                                              final_level, ring);
        const auto levels = FrameGenPyramid(extent);
        if (change != FrameGenHistoryState::kNone && !WaitIdle()) {
            // Recent submissions still use the textures being replaced.
            view.history_state = {};
            error = "D3D12 Frame Generation GPU work did not finish before reallocation";
            return false;
        }
        if (change != FrameGenHistoryState::kNone) {
            // Analyzed frames refer to the textures being replaced.
            analyzed_.clear();
            for (auto& generation : view.slot_generation) ++generation;
        }
        // The depth textures are created last: they mark a view set up.
        if (change & FrameGenHistoryState::kAll || !view.depth[1].resource) {
            view.capture.valid = false;
            const DXGI_FORMAT view_format = ViewFormat(source_desc.Format, submission.source.format);
            view.history = {};
            for (std::uint8_t slot = 0; slot < ring; ++slot) {
                if (!Create(view.history[slot], extent, source_desc.Format, view_format, false, error)) return false;
            }
            // Square-root luma in [0, 1] and -1 where invalid: half floats keep
            // it to a two-thousandth, below what matching costs resolve.
            for (std::uint8_t level = 1; level <= kFrameGenPyramidLevels; ++level) {
                if (!Create(view.luma_newest[level], levels[level], DXGI_FORMAT_R16_FLOAT, DXGI_FORMAT_R16_FLOAT, true, error) ||
                    !Create(view.luma_previous[level], levels[level], DXGI_FORMAT_R16_FLOAT, DXGI_FORMAT_R16_FLOAT, true, error)) {
                    return false;
                }
                if (level >= 2 && !Create(view.flow[level], levels[level], DXGI_FORMAT_R16G16B16A16_FLOAT,
                                          DXGI_FORMAT_R16G16B16A16_FLOAT, true, error)) {
                    return false;
                }
            }
            view.output = {}; // created when first needed (see EnsureOutput)
            view.compose_target = {}; // likewise (see EnsureComposeTarget)
            view.trust = {};  // likewise (see EnsureTrust)
            view.shift_keys = {};
            view.shift_map = {};
            view.motion_map = {};
            view.cross_keys = {};
            view.depth_stale = {};
            view.trust_tiles = {};
            view.trust_votes = {};
            view.depth_scales = {};
            view.depth_scale_raw = {};
            view.depth_scale_votes = {};
            view.trust_stereo = false;
            view.depth = {};
            for (std::uint8_t slot = 0; slot < ring; ++slot) {
                if (!Create(view.depth[slot], levels[1], DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32_FLOAT, true, error)) {
                    return false;
                }
            }
            view.depth_ready = false;
        }
        if (change != FrameGenHistoryState::kNone || !view.filtered[0].resource) {
            view.filtered = {};
            view.filtered_cover = {}; // created when first needed (see EnsureCover)
            for (std::uint8_t slot = 0; slot < ring; ++slot) {
                if (!Create(view.filtered[slot], levels[final_level], DXGI_FORMAT_R16G16B16A16_FLOAT,
                            DXGI_FORMAT_R16G16B16A16_FLOAT, true, error)) {
                    return false;
                }
            }
        }
        if (tuning.analyze_only && tuning.cover && !view.filtered_cover[0].resource) {
            for (std::uint8_t slot = 0; slot < ring; ++slot) {
                if (!Create(view.filtered_cover[slot], levels[final_level], DXGI_FORMAT_R16G16B16A16_FLOAT,
                            DXGI_FORMAT_R16G16B16A16_FLOAT, true, error)) {
                    return false;
                }
            }
        }
        // Engine motion vectors (stage 2): the raw fields the game's vectors
        // are resampled into at half resolution, made here only (CaptureMotion
        // never allocates: the frame after setup goes uncaptured, as color),
        // for the view's grid (a view set up again drops its old ones, the GPU
        // done with them above). They are research data nothing generated
        // reads: when they cannot be made, Frame Generation goes on without
        // them, and engine capture stays off for the session.
        // From Observe (stage 3), with them: each field's census and the
        // evidence passes' textures.
        auto reset_engine = [&view] {
            view.engine_raw = {};
            view.engine_pair = {};
            view.engine_read = {};
            view.engine_next = 0;
            view.engine_pair_next = view.engine_ready = false;
            view.engine_census = {};
            view.engine_count = {};
            view.engine_captures = view.engine_used = 0;
            view.engine_partials = view.engine_params = view.engine_evidence = {};
        };
        if (change & FrameGenHistoryState::kAll) {
            reset_engine();
        }
        const bool observe = tuning.engine_mode >= FrameGenEngineMode::kObserve;
        if (tuning.engine_mode >= FrameGenEngineMode::kCapture && !engine_failed_ &&
            (!view.engine_raw[0].resource || (observe && !view.engine_params.resource))) {
            reset_engine();
            if (!engine_slots_chosen_) {
                engine_slots_chosen_ = true;
                engine_slots_ = engine_slots_switch_.value_or(observe ? 3 : 2);
            }
            std::string why;
            bool made = true;
            for (std::uint8_t slot = 0; slot < engine_slots_ && made; ++slot) {
                made = Create(view.engine_raw[slot], levels[1], engine_format_, engine_format_, true, why) &&
                       (!observe || Create(view.engine_census[slot], {4 * kFrameGenEngineImages, 1},
                                           DXGI_FORMAT_R32G32B32A32_UINT, DXGI_FORMAT_R32G32B32A32_UINT, false, why));
            }
            const std::uint32_t groups = (kFrameGenEngineGrid / kFrameGenThreadGroupSize) *
                                         (kFrameGenEngineGrid / kFrameGenThreadGroupSize);
            made = made && (!observe || (Create(view.engine_partials, {groups * kFrameGenEnginePartialTexels, 1},
                                                DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT, true, why) &&
                                         Create(view.engine_params, {kFrameGenEngineParamsTexels, 1},
                                                DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT, true, why) &&
                                         Create(view.engine_evidence, {kFrameGenEngineEvidenceTexels, 1},
                                                DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT, true, why)));
            if (!made) {
                engine_failed_ = true;
                reset_engine();
                if (motion_notes_.size() < kMotionNotes) {
                    motion_notes_.push_back("the raw fields could not be made (" + why +
                                            "): no capture this session, Frame Generation goes on without it");
                }
            }
        }
        setup.complete = true;
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
        // The driver installs it in System32: only from there, never from
        // the game's folder or the search path (a file planted there would
        // run inside the game, under VectorXR's signature).
        const HMODULE library = LoadLibraryExW(L"nvofapi64.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
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
    void FailAllocationForTest(std::uint32_t ordinal) override { allocation_failure_ = ordinal; }
    void SetHardwareAtlas(bool on) override { atlas_enabled_ = on; }
    bool UsesHardwareAtlasForTest() const override { return hardware_active_ && atlas_in_use_; }

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
            std::array<std::array<UINT64, kWatchedOperations>, kWatchedViews> ready_epoch{};
            while (!hardware_watchdog_stop_) {
                Sleep(50);
                for (std::size_t i = 0; i < kWatchedViews; ++i) {
                    HardwareWatch& watch = hardware_watch_[i];
                    for (std::size_t j = 0; j < kWatchedOperations; ++j) {
                        const HardwareWatch::Snapshot operation = watch.Read(j);
                        ID3D12Fence* fence = operation.fence;
                        const UINT64 done = operation.done;
                        const UINT64 needed = operation.input;
                        const ULONGLONG allowed = operation.allowed;
                        if (!operation || fence->GetCompletedValue() >= done || input->GetCompletedValue() < needed) {
                            ready_since[i][j] = 0;
                            continue;
                        }
                        const ULONGLONG now = GetTickCount64();
                        if (!ready_since[i][j] || ready_done[i][j] != done || ready_epoch[i][j] != operation.epoch) {
                            ready_since[i][j] = now;
                            ready_done[i][j] = done;
                            ready_epoch[i][j] = operation.epoch;
                        } else if (now - ready_since[i][j] > allowed && watch.Current(operation) &&
                                   fence->GetCompletedValue() < done) {
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
    // With `own_session` false, only the images the view packs into and
    // unpacks from: the atlas matches them (EnsureAtlas, RunHardwareAtlas).
    bool EnsureHardware(ViewResources& view, std::size_t index, const FrameGenViewSubmission& submission,
                        const FrameGenTuning& tuning, bool own_session) {
#ifdef DEPTHXR_NVOF
        const FrameGenExtent input = FrameGenPyramid(submission.request.extent)[1];
        const FrameGenExtent flow = FrameGenPyramid(submission.request.extent)[kFrameGenHardwareFlowLevel];
        const NV_OF_PERF_LEVEL perf = HardwarePerf(tuning);
        const bool sized = view.hardware_extent.width == input.width && view.hardware_extent.height == input.height;
        if (own_session ? view.hardware && sized && view.hardware_perf == perf
                        : !view.hardware && view.hardware_newest.resource && sized) {
            return true;
        }
        if ((view.hardware || view.hardware_newest.resource) && !WaitIdle()) {
            return HardwareFailed("the GPU did not finish before the optical flow session was resized");
        }
        if (hardware_hung_) {
            // The watchdog released a stalled match (perhaps during that
            // drain): no new session on that hardware.
            return HardwareFailed("the optical flow hardware stopped responding");
        }
        RetireHardware(view);
        if (!own_session) {
            std::string error;
            if (!Create(view.hardware_newest, input, DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8_UNORM, true, error, true) ||
                !Create(view.hardware_previous, input, DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8_UNORM, true, error, true) ||
                !Create(view.hardware_flow, flow, DXGI_FORMAT_R16G16_SINT, DXGI_FORMAT_R16G16_SINT, false, error, true)) {
                return HardwareFailed("its images could not be created (" + error + ")");
            }
            view.hardware_extent = input;
            view.hardware_fresh = true;
            view.hardware_matched = view.raw_matched = false;
            view.hardware_mode = 0;
            view.raw_previous = false;
            view.raw_value = 0;
            view.raw_captured = view.raw_executed = 0;
            return true;
        }
        // The hardware reads and writes them in COMMON, to which the work
        // before it returns them (RunHardwareFlow).
        std::string error;
        // Created in COMMON, the state the hardware's own work (on its video
        // queue) expects them in from registration on.
        if (!Create(view.hardware_newest, input, DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8_UNORM, true, error, true) ||
            !Create(view.hardware_previous, input, DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8_UNORM, true, error, true) ||
            !Create(view.hardware_flow, flow, DXGI_FORMAT_R16G16_SINT, DXGI_FORMAT_R16G16_SINT, false, error, true) ||
            !Create(view.raw[0], input, DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8_UNORM, true, error, true) ||
            !Create(view.raw[1], input, DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8_UNORM, true, error, true) ||
            !Create(view.raw_flow, flow, DXGI_FORMAT_R16G16_SINT, DXGI_FORMAT_R16G16_SINT, false, error, true)) {
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
            if (!hardware_watch_[index].ResetFence(view.hardware_fence.Get())) {
                RetireHardware(view);
                return HardwareFailed("the previous optical flow session still has unfinished work");
            }
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
        static_cast<void>(own_session);
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
        // Values of the fence just retired: a new session's fence starts
        // over, and a game-queue wait for the old value would never end.
        view.raw_last_match = 0;
        view.raw_reference = 0;
    }

#ifdef DEPTHXR_NVOF
    static NV_OF_PERF_LEVEL HardwarePerf(const FrameGenTuning& tuning) {
        return tuning.quality == FrameGenFlowQuality::kPerformance ? NV_OF_PERF_LEVEL_FAST : NV_OF_PERF_LEVEL_MEDIUM;
    }

    // The atlas session for `views`' packed images: two views a row (see
    // the layouts below), each at a multiple of 8 texels (whole cells of the hardware's
    // 4-texel vectors) with 64 texels between them, left black (committed
    // textures start zeroed) so matches stay within a view. False when the
    // hardware refuses it (too large, or anything else): sessions per view.
    bool EnsureAtlas(const std::vector<FrameGenViewSubmission>& views, const FrameGenTuning& tuning) {
        const NV_OF_PERF_LEVEL perf = HardwarePerf(tuning);
        std::vector<FrameGenExtent> inputs;
        for (const FrameGenViewSubmission& view : views) {
            inputs.push_back(FrameGenPyramid(view.request.extent)[1]);
        }
        auto same_inputs = [&](const HardwareAtlas& atlas) {
            return atlas.session && atlas.inputs.size() == inputs.size() &&
                   std::equal(inputs.begin(), inputs.end(), atlas.inputs.begin(), [](FrameGenExtent a, FrameGenExtent b) {
                       return a.width == b.width && a.height == b.height;
                   });
        };
        auto laid_out = [&](const HardwareAtlas& atlas) { return atlas.perf == perf && same_inputs(atlas); };
        if (laid_out(atlas_)) {
            return true;
        }
        if (hardware_watch_[0].Pending()) {
            // The current atlas's match is outstanding, and the next frame's
            // input may be queued behind its wait: replacing its watch now
            // would leave nothing to release that wait if the match stalled.
            // Drain first, with the watch in place so the watchdog can still
            // release a stall, as EnsureHardware drains before replacing a
            // view's session. A quality or layout change costs one drain and
            // applies on this frame (deferring it could last indefinitely
            // while matches overlap). A drain that times out is no refusal
            // (Execute retries next frame).
            if (!WaitIdle()) {
                atlas_busy_ = true;
                return false;
            }
            // The watchdog released a stalled match meanwhile: the hardware
            // is not used again (Execute falls back to shader flow).
            if (hardware_hung_) return HardwareFailed("the optical flow hardware stopped responding");
        }
        // Another layout's atlas becomes the current one, kept; one laid out
        // so already comes back. Nothing is pending on the current atlas here.
        if (atlas_.session) {
            atlas_cache_.push_back(std::move(atlas_));
        }
        atlas_ = {};
        const auto cached = std::find_if(atlas_cache_.begin(), atlas_cache_.end(), laid_out);
        if (cached != atlas_cache_.end()) {
            atlas_ = std::move(*cached);
            atlas_cache_.erase(cached);
            atlas_.fresh = true; // its hints are from another sequence of frames
            if (!WatchAtlas()) return false;
            TrimAtlases();
            return true;
        }
        constexpr UINT kGutter = 64;
        auto align = [](UINT value) { return (value + 7u) & ~7u; };
        // Rows of views: in order two a row, or (four views) each row a
        // periphery with a focus view, which wastes less (a quad views
        // atlas 10% smaller); the smallest that fits the hardware.
        std::vector<std::vector<std::vector<std::size_t>>> layouts;
        std::vector<std::vector<std::size_t>> in_order;
        for (std::size_t row = 0; row < inputs.size(); row += 2) {
            in_order.push_back(row + 1 < inputs.size() ? std::vector<std::size_t>{row, row + 1} : std::vector<std::size_t>{row});
        }
        layouts.push_back(in_order);
        if (inputs.size() == 4) {
            layouts.push_back({{0, 2}, {1, 3}});
            layouts.push_back({{0, 3}, {1, 2}});
        }
        if (inputs.size() == 2) {
            layouts.push_back({{0}, {1}}); // one above the other, for views too wide side by side
        }
        constexpr UINT kHardwareMax = 8192;
        std::vector<std::array<UINT, 2>> cells(inputs.size());
        UINT width = 0;
        UINT height = 0;
        bool chosen = false;
        for (const auto& rows : layouts) {
            std::vector<std::array<UINT, 2>> layout_cells(inputs.size());
            UINT layout_width = 0;
            UINT layout_height = 0;
            for (const auto& row : rows) {
                UINT x = 0;
                UINT row_height = 0;
                for (const std::size_t i : row) {
                    layout_cells[i] = {x, layout_height};
                    x += align(inputs[i].width) + kGutter;
                    row_height = std::max(row_height, align(inputs[i].height));
                }
                layout_width = std::max(layout_width, x - kGutter);
                layout_height += row_height + kGutter;
            }
            layout_height -= kGutter;
            const bool fits = layout_width <= kHardwareMax && layout_height <= kHardwareMax;
            const bool smaller = std::uint64_t{layout_width} * layout_height < std::uint64_t{width} * height;
            if (fits && (!chosen || smaller)) {
                cells = layout_cells;
                width = layout_width;
                height = layout_height;
                chosen = true;
            }
        }
        if (!chosen) {
            return false; // too large for one image: sessions per view
        }
        std::string error;
        const FrameGenExtent extent{width, height};
        const FrameGenExtent flow{(width + 3) / 4, (height + 3) / 4};
        if (!Create(atlas_.newest, extent, DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8_UNORM, false, error, true) ||
            !Create(atlas_.previous, extent, DXGI_FORMAT_R8_UNORM, DXGI_FORMAT_R8_UNORM, false, error, true) ||
            !Create(atlas_.flow, flow, DXGI_FORMAT_R16G16_SINT, DXGI_FORMAT_R16G16_SINT, false, error, true)) {
            RetireAtlas(atlas_);
            return false;
        }
        if (hardware_api_.nvCreateOpticalFlowD3D12(device_.Get(), &atlas_.session) != NV_OF_SUCCESS || !atlas_.session) {
            atlas_.session = {};
            RetireAtlas(atlas_);
            return false;
        }
        NV_OF_INIT_PARAMS init{};
        init.width = width;
        init.height = height;
        init.outGridSize = NV_OF_OUTPUT_VECTOR_GRID_SIZE_4;
        init.hintGridSize = NV_OF_HINT_VECTOR_GRID_SIZE_UNDEFINED;
        init.mode = NV_OF_MODE_OPTICALFLOW;
        init.perfLevel = perf;
        init.predDirection = NV_OF_PRED_DIRECTION_FORWARD;
        init.inputBufferFormat = NV_OF_BUFFER_FORMAT_GRAYSCALE8;
        if (hardware_api_.nvOFInit(atlas_.session, &init) != NV_OF_SUCCESS ||
            FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&atlas_.fence)))) {
            RetireAtlas(atlas_);
            return false;
        }
        Texture* images[3] = {&atlas_.newest, &atlas_.previous, &atlas_.flow};
        for (int i = 0; i < 3; ++i) {
            NV_OF_REGISTER_RESOURCE_PARAMS_D3D12 params{};
            params.resource = images[i]->resource.Get();
            params.inputFencePoint = {atlas_.fence.Get(), atlas_.fence_value};
            params.hOFGpuBuffer = &atlas_.buffers[i];
            params.outputFencePoint = {atlas_.fence.Get(), ++atlas_.fence_value};
            if (hardware_api_.nvOFRegisterResourceD3D12(atlas_.session, &params) != NV_OF_SUCCESS) {
                RetireAtlas(atlas_);
                return false;
            }
        }
        atlas_.perf = perf;
        atlas_.inputs = inputs;
        atlas_.cells = cells;
        atlas_.fresh = true;
        atlas_.matched = false;
        if (!WatchAtlas()) return false;
        TrimAtlases();
        return true;
    }

    // The watchdog watches the current atlas as view 0 (operations of
    // another fence never carry over to it).
    bool WatchAtlas() { return hardware_watch_[0].ResetFence(atlas_.fence.Get()); }

    // Frees kept atlases beyond the newest three once nothing submitted
    // can use them (never waits).
    void TrimAtlases() {
        constexpr std::size_t kKeptAtlases = 3;
        for (auto it = atlas_cache_.begin(); atlas_cache_.size() > kKeptAtlases && it != atlas_cache_.end();) {
            const bool done = (!it->fence || it->fence->GetCompletedValue() >= it->fence_value) &&
                              fence_->GetCompletedValue() >= fence_value_;
            if (done) {
                RetireAtlas(*it);
                it = atlas_cache_.erase(it);
            } else {
                ++it;
            }
        }
    }

    void RetireAtlases() {
        RetireAtlas(atlas_);
        for (HardwareAtlas& atlas : atlas_cache_) {
            RetireAtlas(atlas);
        }
        atlas_cache_.clear();
    }

    // Ends the atlas session, as RetireHardware a view's: images the driver
    // was given are never freed.
    void RetireAtlas(HardwareAtlas& atlas) {
        ID3D12Fence* const fence = atlas.fence.Get();
        if (atlas.session && !hardware_hung_) {
            for (NvOFGPUBufferHandle& buffer : atlas.buffers) {
                if (buffer) {
                    NV_OF_UNREGISTER_RESOURCE_PARAMS_D3D12 params{buffer};
                    hardware_api_.nvOFUnregisterResourceD3D12(&params);
                }
            }
            hardware_api_.nvOFDestroy(atlas.session);
        }
        if (atlas.session) {
            for (Texture* texture : {&atlas.newest, &atlas.previous, &atlas.flow}) {
                texture->resource.Detach();
            }
            // The watchdog may still look at it: a fence that is never freed.
            atlas.fence.Detach();
        }
        if (fence && hardware_watch_[0].ActiveFence() == fence) {
            // Refused while its work is unfinished: the watch stays on the
            // fence, which is never freed (detached above), so a stall can
            // still be released.
            hardware_watch_[0].ResetFence(nullptr);
        }
        atlas = {};
    }

    // RunHardwareFlow on the atlas: copies every waiting view's packed images
    // into its cell, submits, has the hardware match the atlas once, makes
    // the queue wait for the vectors, reopens the command list and copies
    // each view's vectors out of its cell into the view's own image.
    bool RunHardwareAtlas(std::vector<ViewRecording>& recordings, FrameSlot& slot, std::size_t slot_index,
                          std::string& error) {
        std::vector<D3D12_RESOURCE_BARRIER> barriers;
        auto waiting = [&](std::size_t index) {
            return recordings[index].waits_for_hardware && index < atlas_.cells.size();
        };
        for (std::size_t index = 0; index < recordings.size(); ++index) {
            if (!waiting(index)) continue;
            Require(barriers, recordings[index].view->hardware_newest, D3D12_RESOURCE_STATE_COPY_SOURCE);
            Require(barriers, recordings[index].view->hardware_previous, D3D12_RESOURCE_STATE_COPY_SOURCE);
        }
        Require(barriers, atlas_.newest, D3D12_RESOURCE_STATE_COPY_DEST);
        Require(barriers, atlas_.previous, D3D12_RESOURCE_STATE_COPY_DEST);
        Flush(barriers);
        bool temporal = !atlas_.fresh;
        for (std::size_t index = 0; index < recordings.size(); ++index) {
            if (!waiting(index)) continue;
            ViewResources& view = *recordings[index].view;
            temporal = temporal && recordings[index].temporal && !view.hardware_fresh;
            const auto [x, y] = atlas_.cells[index];
            for (const auto& [from, to] : {std::pair{&view.hardware_newest, &atlas_.newest},
                                           std::pair{&view.hardware_previous, &atlas_.previous}}) {
                D3D12_TEXTURE_COPY_LOCATION destination{to->resource.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
                D3D12_TEXTURE_COPY_LOCATION source{from->resource.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
                destination.SubresourceIndex = source.SubresourceIndex = 0;
                commands_->CopyTextureRegion(&destination, x, y, 0, &source, nullptr);
            }
        }
        // The hardware reads and writes its images in COMMON.
        Require(barriers, atlas_.newest, D3D12_RESOURCE_STATE_COMMON);
        Require(barriers, atlas_.previous, D3D12_RESOURCE_STATE_COMMON);
        Require(barriers, atlas_.flow, D3D12_RESOURCE_STATE_COMMON);
        Flush(barriers);
        Stamp(kProfileHardware);
        const UINT first = static_cast<UINT>(slot_index * kTimingStamps);
        if (query_heap_) commands_->EndQuery(query_heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, first + 1);
        HRESULT hr = commands_->Close();
        if (FAILED(hr)) {
            error = Failure("D3D12 Frame Generation command close", hr);
            return false;
        }
        hr = SubmitWork(commands_.Get());
        for (const ViewRecording& recording : recordings) {
            MarkUsed(*recording.view, fence_.Get(), fence_value_);
        }
        if (FAILED(hr)) {
            error = Failure("D3D12 Frame Generation queue signal", hr);
            signal_failed_ = true;
            return false;
        }
        NV_OF_FENCE_POINT ready{fence_.Get(), fence_value_};
        NV_OF_EXECUTE_INPUT_PARAMS_D3D12 in{};
        in.inputFrame = atlas_.buffers[0];
        in.referenceFrame = atlas_.buffers[1];
        in.disableTemporalHints = temporal ? NV_OF_FALSE : NV_OF_TRUE;
        in.numFencePoints = 1;
        in.fencePoint = &ready;
        NV_OF_FENCE_POINT done{atlas_.fence.Get(), ++atlas_.fence_value};
        NV_OF_EXECUTE_OUTPUT_PARAMS_D3D12 out{};
        out.outputBuffer = atlas_.buffers[2];
        out.fencePoint = &done;
        hardware_watch_[0].Watch(done.value, ready.value, atlas_.matched ? 250 : 3000);
        const auto call = std::chrono::steady_clock::now();
        if (stall_hardware_for_test_) {
            stall_hardware_for_test_ = false; // its fence is never signalled
        } else if (hardware_api_.nvOFExecuteD3D12(atlas_.session, &in, &out) != NV_OF_SUCCESS) {
            // This frame keeps the last vectors; later frames match on the
            // shader cores again.
            HardwareFailed("the optical flow hardware failed: " + HardwareError(atlas_.session));
            work_->Signal(atlas_.fence.Get(), done.value);
        }
        slot.hardware_call_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - call).count();
        atlas_.fresh = false;
        atlas_.matched = true;
        for (std::size_t index = 0; index < recordings.size(); ++index) {
            if (!waiting(index)) continue;
            ViewResources& view = *recordings[index].view;
            view.hardware_fresh = false;
            view.hardware_matched = true;
            view.hardware_mode = 1;
        }
        work_->Wait(atlas_.fence.Get(), done.value);
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
        if (query_heap_) {
            commands_->EndQuery(query_heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, first + 2);
            slot.hardware_timed = true;
        }
        // Each view's vectors, out of its cell.
        Require(barriers, atlas_.flow, D3D12_RESOURCE_STATE_COPY_SOURCE);
        for (std::size_t index = 0; index < recordings.size(); ++index) {
            if (waiting(index)) Require(barriers, recordings[index].view->hardware_flow, D3D12_RESOURCE_STATE_COPY_DEST);
        }
        Flush(barriers);
        for (std::size_t index = 0; index < recordings.size(); ++index) {
            if (!waiting(index)) continue;
            Texture& flow = recordings[index].view->hardware_flow;
            const D3D12_RESOURCE_DESC desc = flow.resource->GetDesc();
            const auto [x, y] = atlas_.cells[index];
            const D3D12_BOX box{x / 4, y / 4, 0, x / 4 + static_cast<UINT>(desc.Width), y / 4 + desc.Height, 1};
            D3D12_TEXTURE_COPY_LOCATION destination{flow.resource.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
            D3D12_TEXTURE_COPY_LOCATION source{atlas_.flow.resource.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
            destination.SubresourceIndex = source.SubresourceIndex = 0;
            commands_->CopyTextureRegion(&destination, 0, 0, 0, &source, &box);
        }
        Require(barriers, atlas_.flow, D3D12_RESOURCE_STATE_COMMON);
        Flush(barriers);
        return true;
    }
#else
    bool EnsureAtlas(const std::vector<FrameGenViewSubmission>&, const FrameGenTuning&) { return false; }
    void RetireAtlases() {}
#endif

    // Submits the work recorded so far, has the optical flow hardware match
    // every view waiting for it, makes the queue wait for its vectors and
    // reopens the command list for the rest of the frame.
    bool RunHardwareFlow(std::vector<ViewRecording>& recordings, FrameSlot& slot, std::size_t slot_index,
                         std::string& error) {
#ifdef DEPTHXR_NVOF
        if (atlas_in_use_) {
            return RunHardwareAtlas(recordings, slot, slot_index, error);
        }
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
        // The analysis' timing: from here until the work after the hardware
        // starts, it waits for the hardware.
        const UINT first = static_cast<UINT>(slot_index * kTimingStamps);
        if (query_heap_) commands_->EndQuery(query_heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, first + 1);
        HRESULT hr = commands_->Close();
        if (FAILED(hr)) {
            error = Failure("D3D12 Frame Generation command close", hr);
            return false;
        }
        hr = SubmitWork(commands_.Get());
        for (const ViewRecording& recording : recordings) {
            MarkUsed(*recording.view, fence_.Get(), fence_value_);
        }
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
                work_->Wait(view.hardware_fence.Get(), wait);
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
            const auto call = std::chrono::steady_clock::now();
            if (stall_hardware_for_test_) {
                stall_hardware_for_test_ = false; // its fence is never signalled
            } else if (hardware_api_.nvOFExecuteD3D12(view.hardware, &in, &out) != NV_OF_SUCCESS) {
                // This frame keeps the last vectors; later frames match on
                // the shader cores again.
                HardwareFailed("the optical flow hardware failed: " + HardwareError(view.hardware));
                work_->Signal(view.hardware_fence.Get(), wait);
            }
            slot.hardware_call_ms +=
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - call).count();
            view.hardware_fresh = false;
            view.hardware_matched = true;
            view.hardware_mode = 1;
            work_->Wait(view.hardware_fence.Get(), wait);
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
        if (query_heap_) {
            commands_->EndQuery(query_heap_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, first + 2);
            slot.hardware_timed = true;
        }
        return true;
#else
        static_cast<void>(recordings);
        static_cast<void>(slot);
        static_cast<void>(slot_index);
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
        // Recording Synthesize: the analyzed frame's slots.
        const std::uint8_t newest =
            view.bound_newest >= 0 ? static_cast<std::uint8_t>(view.bound_newest) : view.history_state.Newest();
        const std::uint8_t previous =
            view.bound_previous >= 0 ? static_cast<std::uint8_t>(view.bound_previous) : view.history_state.Previous();
        switch (texture.kind) {
        case FrameGenTextureKind::kHistoryNewest: return &view.history[newest];
        case FrameGenTextureKind::kHistoryPrevious: return &view.history[previous];
        case FrameGenTextureKind::kLumaNewest: return &view.luma_newest[texture.level];
        case FrameGenTextureKind::kLumaPrevious: return &view.luma_previous[texture.level];
        case FrameGenTextureKind::kFlow: return &view.flow[texture.level];
        case FrameGenTextureKind::kFilteredNewest: return &view.filtered[newest];
        case FrameGenTextureKind::kFilteredPrevious: return &view.filtered[previous];
        case FrameGenTextureKind::kFilteredCover: return &view.filtered_cover[newest];
        case FrameGenTextureKind::kOutput: return &view.output;
        case FrameGenTextureKind::kDepth: return &view.depth[newest];
        case FrameGenTextureKind::kDepthPrevious: return &view.depth[previous];
        case FrameGenTextureKind::kHardwareNewest: return &view.hardware_newest;
        case FrameGenTextureKind::kHardwarePrevious: return &view.hardware_previous;
        case FrameGenTextureKind::kHardwareFlow: return view.raw_in_use ? &view.raw_flow : &view.hardware_flow;
        case FrameGenTextureKind::kTrustNewest: return &view.trust[newest];
        case FrameGenTextureKind::kTrustPrevious: return &view.trust[previous];
        case FrameGenTextureKind::kShiftKeys: return &view.shift_keys;
        case FrameGenTextureKind::kShiftMap: return &view.shift_map;
        case FrameGenTextureKind::kDepthStale: return &view.depth_stale;
        case FrameGenTextureKind::kMotionMap: return &view.motion_map;
        case FrameGenTextureKind::kWorldPartials: return &view.world_partials;
        case FrameGenTextureKind::kWorldParams: return &view.world_params;
        case FrameGenTextureKind::kTrustTiles: return &view.trust_tiles;
        case FrameGenTextureKind::kTrustVotes: return &view.trust_votes;
        case FrameGenTextureKind::kDepthScales: return &view.depth_scales;
        case FrameGenTextureKind::kDepthScaleVotes: return &view.depth_scale_votes;
        case FrameGenTextureKind::kDepthScaleRaw: return &view.depth_scale_raw;
        case FrameGenTextureKind::kEngineRaw: return &view.engine_raw[EngineNewest(view)];
        case FrameGenTextureKind::kEngineRawPrevious: return &view.engine_raw[EnginePrevious(view)];
        case FrameGenTextureKind::kEngineCensus: return &view.engine_census[EngineNewest(view)];
        case FrameGenTextureKind::kEngineCensusPrevious: return &view.engine_census[EnginePrevious(view)];
        case FrameGenTextureKind::kEnginePartials: return &view.engine_partials;
        case FrameGenTextureKind::kEngineParams: return &view.engine_params;
        case FrameGenTextureKind::kEngineEvidence: return &view.engine_evidence;
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
        if (texture.state == D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE &&
            (state == D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE || state == D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)) {
            return; // already readable that way
        }
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
    // `evidence`: the evidence on every view's engine motion vectors is taken
    // (Execute checked them all).
    bool BeginView(const FrameGenViewSubmission& submission, const FrameGenTuning& tuning, ViewResources& view,
                   ViewRecording& recording, bool& synthesized, bool& depth_used, bool evidence, std::string& error) {
        const auto source_it = images_.find(submission.source.swapchain);
        if (source_it == images_.end() || submission.source.image_index >= source_it->second.size() ||
            (!tuning.analyze_only && submission.outputs.size() < tuning.generated)) {
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
            // Only with SetCopyAfterRelease (Uncaptured stops Execute otherwise).
            ++view.slot_generation[view.history_state.Newest()];
            source_copied_ = true;
            CopySource(commands_.Get(), barriers, view, submission.source, source);
            Stamp(kProfileSourceCopy);
        }

        // Depth only when it was captured for this frame; it is used once.
        FrameGenViewRequest effective = view.history_state.Effective(submission.request);
        // Only once the depth is known to line the eyes up (see RecordDepthCheck).
        effective.depth = effective.depth && view.depth_ready && view.depth[1].resource && validator_.Usable() &&
                          view.depth_generation == validator_.Generation();
        view.depth_ready = false;
        // Engine motion vectors likewise: captured for this frame (paired
        // with its color capture), used once.
        const std::uint8_t engine_newest = EngineNewest(view);
        recording.engine = view.engine_ready && view.engine_raw[engine_newest].resource &&
                           view.engine_pair[engine_newest].Matches(submission.source);
        view.engine_ready = false;
        motion_paired_ += recording.engine ? 1 : 0;
        // Stage 3: their evidence (every view's or none), with how they
        // decode, the images the newest and previous fields came from, and
        // whether the field before is the frame before's (the frame the last
        // Execute took, captured right before this one).
        effective.engine_motion = evidence && recording.engine;
        if (effective.engine_motion) {
            const std::uint8_t engine_previous = EnginePrevious(view);
            effective.engine_encoding = view.engine_encoding[engine_newest];
            effective.engine_images = {view.engine_image[engine_newest], view.engine_image[engine_previous]};
            effective.engine_vectors = view.engine_vectors[engine_newest];
            effective.engine_previous = view.engine_count[engine_previous] != 0 &&
                                        view.engine_count[engine_previous] + 1 == view.engine_count[engine_newest] &&
                                        view.engine_used == view.engine_count[engine_previous];
        }
        view.engine_used = recording.engine ? view.engine_count[engine_newest] : 0;
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
        if (effective.depth && (tuning.depth_splat || depth_splat_) && !view.motion_map.resource &&
            !Create(view.motion_map, FrameGenPyramid(submission.request.extent)[1], DXGI_FORMAT_R32_UINT,
                    DXGI_FORMAT_R32_UINT, true, error)) {
            return false;
        }
        // The previous frame's depth was used (its trust scores were kept),
        // so it is still in the other depth image.
        effective.depth_previous = effective.depth && effective.trust_history;
        // World content moves with the world's fitted motion.
        effective.world_motion = world_motion_ && effective.depth && effective.synthesize;
        if (effective.world_motion && !EnsureWorld(view, error)) {
            return false;
        }
        depth_used = effective.depth && effective.synthesize;
        recording.submission = &submission;
        recording.view = &view;
        recording.temporal = effective.temporal;
        recording.request = effective;
        recording.plan = BuildFrameGenViewPlan(effective, tuning);
        const FrameGenViewPlan& plan = recording.plan;
        recording.evidence = effective.engine_motion && plan.synthesize;
        synthesized = plan.synthesize;
        if (tuning.analyze_only) {
            return true; // Synthesize writes the frames
        }
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
                         CanDraw(submission, outputs, frames.size(), plan.frames_in_order);
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
        // Frames written in plan order (each eye-shift map is replaced by the
        // next): the frames after one map, drawn together before the next.
        std::vector<std::size_t> batch_indices;
        std::vector<const FrameGenDispatch*> batch;
        auto flush_batch = [&] {
            const bool ok = batch.empty() || RecordDraw(view, submission, outputs, batch_indices, batch, error);
            batch.clear();
            batch_indices.clear();
            return ok;
        };
        while (recording.next < recording.plan.dispatches.size()) {
            if (until_hardware && recording.plan.dispatches[recording.next].pass == FrameGenPass::kFlowUnpack) {
                // The optical flow hardware's vectors are next.
                recording.waits_for_hardware = true;
                return flush_batch();
            }
            const FrameGenDispatch& dispatch = recording.plan.dispatches[recording.next++];
            if (dispatch.pass == FrameGenPass::kShiftClear && clear_heap_) {
                // The eye-shift keys to nothing reached and the map to no
                // offset (FG_PASS_SHIFT_CLEAR's values), cleared in hardware.
                if (!flush_batch()) {
                    return false;
                }
                const UINT values[2][4] = {{0u, 0u, 0u, 0u}, {0xffffffffu, 0xffffffffu, 0xffffffffu, 0xffffffffu}};
                for (UINT i = 0; i < 2; ++i) {
                    Texture* texture = Resolve(view, dispatch.outputs[i]);
                    if (!texture) {
                        continue;
                    }
                    if (descriptor_cursor_ + 1 > descriptor_end_) {
                        error = "D3D12 Frame Generation descriptor budget exhausted";
                        return false;
                    }
                    const UINT gpu_index = descriptor_cursor_++;
                    UnorderedAccess(texture, gpu_index);
                    D3D12_CPU_DESCRIPTOR_HANDLE cpu = clear_heap_->GetCPUDescriptorHandleForHeapStart();
                    cpu.ptr += static_cast<SIZE_T>(clear_cursor_++ % kClearViews) * descriptor_size_;
                    D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
                    uav.Format = texture->view_format;
                    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                    device_->CreateUnorderedAccessView(texture->resource.Get(), nullptr, &uav, cpu);
                    Require(barriers, *texture, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
                    Flush(barriers);
                    commands_->ClearUnorderedAccessViewUint(Gpu(gpu_index), cpu, texture->resource.Get(), values[i], 0,
                                                            nullptr);
                }
                Stamp(static_cast<int>(dispatch.pass));
                continue;
            }
            if (recording.draw && Generates(dispatch)) {
                if (recording.plan.frames_in_order) {
                    // Drawn together only with frames reading the same map
                    // the same way (the shift flag and the map's time).
                    if (!batch.empty() && (batch.front()->constants.options[1] != dispatch.constants.options[1] ||
                                           batch.front()->constants.options[3] != dispatch.constants.options[3])) {
                        if (!flush_batch()) return false;
                    }
                    for (std::size_t index = 0; index < outputs.size(); ++index) {
                        if (dispatch.copy_mask & (1u << index)) {
                            batch_indices.push_back(index);
                            batch.push_back(&dispatch);
                        }
                    }
                }
                continue;
            }
            if (!flush_batch()) {
                return false;
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
        if (!recording.draw || recording.plan.frames_in_order) {
            return flush_batch();
        }
        std::vector<std::size_t> indices(recording.frames.size());
        for (std::size_t i = 0; i < indices.size(); ++i) {
            indices[i] = i;
        }
        return RecordDraw(view, submission, outputs, indices, recording.frames, error);
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
    ComPtr<ID3D12CommandQueue> queue_; // the game's (session) queue: captures, and waits before output releases
    // Frame Generation's own work: a high-priority queue of its own, or the
    // game's queue (`separate_` false).
    ComPtr<ID3D12CommandQueue> work_;
    bool separate_{false};
    // The queue the runtime synchronizes released images with: its own
    // (FrameGenRuntimeQueue, `runtime_on_present_`) or the game's. Frames
    // written just in time are submitted to it; it signals present_fence_
    // when it is neither the work queue nor the game's.
    ComPtr<ID3D12CommandQueue> present_;
    bool runtime_on_present_{false};
    ComPtr<ID3D12Fence> present_fence_;
    UINT64 present_value_{0};
    // Captures signal this on the game's queue while separate (fence_ otherwise).
    ComPtr<ID3D12Fence> capture_fence_;
    UINT64 capture_value_{0};
    UINT64 capture_waited_{0}; // the capture value the work queue last waited for
    bool just_in_time_{true};  // SupportsJustInTime
    bool source_copied_{false}; // this Execute copied an application image itself (SetCopyAfterRelease)
    // Just-in-time synthesis: its own command list and allocators, and the
    // real frames analyzed for it (newest last).
    struct SynthesisSlot {
        ComPtr<ID3D12CommandAllocator> allocator;
        ID3D12Fence* fence{nullptr}; // the fence its submission signals
        UINT64 fence_value{0};
    };
    std::array<SynthesisSlot, kSynthesisSlots> synthesis_slots_;
    ComPtr<ID3D12GraphicsCommandList> synthesis_commands_;
    ComPtr<ID3D12GraphicsCommandList5> synthesis_commands5_;
    std::uint64_t synthesis_count_{0};
    std::deque<AnalyzedFrame> analyzed_;
    std::uint64_t next_analyzed_id_{1};
    ComPtr<ID3D12RootSignature> root_signature_;
    std::array<ComPtr<ID3D12PipelineState>, kFrameGenPassCount> pipelines_;
    ComPtr<ID3D12DescriptorHeap> heap_;
    UINT descriptor_size_{0};
    UINT descriptor_cursor_{0};
    UINT descriptor_end_{0};
    ComPtr<ID3D12GraphicsCommandList> commands_;
    ComPtr<ID3D12GraphicsCommandList5> commands5_; // the same list, for variable-rate shading
    UINT shading_tile_{0};                          // pixels per shading-rate tile side; 0: no variable-rate shading
    bool world_motion_{true};                       // FrameGenViewRequest::world_motion where depth is in use
    bool occlusion_{true};                          // FrameGenTuning::occlusion allowed
    bool cross_fade_{false};                        // FrameGenTuning::cross_fade forced (research)
    bool depth_match_{false};                       // FrameGenTuning::depth_match forced (research)
    bool depth_splat_{false};                       // FrameGenTuning::depth_splat forced (research)
    std::array<FrameSlot, kFramesInFlight> slots_;
    ComPtr<ID3D12GraphicsCommandList> capture_commands_;
    std::array<CaptureSlot, kCaptureSlots> capture_slots_; // views x frames in flight, color and depth
    ComPtr<IDXGIAdapter3> memory_adapter_; // for MemoryBudget
    Texture check_scores_;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT check_footprint_{};
    UINT64 check_bytes_{0};
    FrameGenDepthValidator validator_;
    FrameGenDepthEyes eyes_; // which eye's depth is checked, and whether an eye takes the other's
    bool depth_eyes_{true};   // eyes_ judges the checks (VECTORXR_FRAMEGEN_DEPTH_EYES)
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
#ifdef DEPTHXR_NVOF
    HardwareAtlas atlas_;
    // Atlases of other layouts (quad views and a canvas in turn), kept for
    // when their layout comes back, as views keep their sessions. Switching
    // drains first while the current atlas's match is outstanding, so its
    // watch is never replaced early. Freed once the GPU is done with them,
    // beyond the newest few, or at Reset.
    std::vector<HardwareAtlas> atlas_cache_;
#endif
    // VECTORXR_FRAMEGEN_HARDWARE_ATLAS=0 (or SetHardwareAtlas): a session
    // per view instead of the atlas.
    bool atlas_enabled_{!(std::getenv("VECTORXR_FRAMEGEN_HARDWARE_ATLAS") &&
                          std::string(std::getenv("VECTORXR_FRAMEGEN_HARDWARE_ATLAS")) == "0")};
    bool atlas_failed_{false}; // the hardware refused the atlas: sessions per view from then on
    bool atlas_busy_{false};   // EnsureAtlas: the drain before a switch timed out (one frame)
    bool atlas_in_use_{false}; // this Execute matches on the atlas
    std::atomic<ULONGLONG> hardware_allowed_ms_{250};
    bool hardware_active_{false};  // the last Execute used the hardware: captures match early
    std::atomic<bool> hardware_stuck_{false};
    std::atomic<bool> hardware_hung_{false}; // the watchdog ever released a fence
    bool stall_hardware_for_test_{false};
    std::uint32_t allocation_failure_{0}; // one-shot ordinal among Create calls, tests only
    // Generated frames drawn straight into the output images.
    static constexpr UINT kRenderTargetViews = 16;
    // Hardware clears of the eye-shift textures (CPU-side views).
    static constexpr UINT kClearViews = 64;
    ComPtr<ID3D12DescriptorHeap> clear_heap_;
    UINT clear_cursor_{0};
    bool direct_output_{true};
    bool copy_after_release_{false}; // SetCopyAfterRelease
    ComPtr<ID3D12DescriptorHeap> rtv_heap_;
    UINT rtv_size_{0};
    UINT rtv_cursor_{0};
    std::unordered_map<std::uint64_t, ComPtr<ID3D12PipelineState>> draw_pipelines_;
    // Quad views: the compose pipeline per output format, the flow merge,
    // and whether the backend composes (SupportsComposition). Generated
    // frames of a periphery skip variable-rate shading (coarse_shading_ off).
    std::unordered_map<DXGI_FORMAT, ComPtr<ID3D12PipelineState>> compose_pipelines_;
    ComPtr<ID3D12PipelineState> flow_merge_pipeline_;
    bool supports_composition_{false};
    bool coarse_shading_{true};
    bool profiling_{false};
    ComPtr<ID3D12QueryHeap> profile_heap_;
    ComPtr<ID3D12Resource> profile_readback_;
    std::vector<int> profile_labels_; // category of the work before each stamp after the first
    std::vector<double> profile_ms_;
    UINT64 capture_count_{0};
    bool stalled_{false}; // torn down with work outstanding (LeakAll)
    ComPtr<ID3D12Fence> fence_;
    UINT64 fence_value_{0};
    UINT64 submission_{0};
    HANDLE event_{nullptr};
    // Read by WaitCapture on other threads.
    std::atomic<bool> signal_failed_{false};
    ComPtr<ID3D12QueryHeap> query_heap_;
    ComPtr<ID3D12Resource> readback_;
    UINT64 timestamp_frequency_{0};
    double last_gpu_ms_{-1.0};
    std::optional<FrameGenGpuWindow> last_window_;
    // GPU-aware pacing: the fence value of the newest capture (CaptureMark)
    // and the event WaitCapture waits on.
    UINT64 last_capture_value_{0};
    HANDLE capture_wait_event_{nullptr};
    // Analyses' timings (TakeTimings), with when the captures they waited
    // for finished: captures are timed on the game's queue, read once done
    // into a ring by fence value.
    std::vector<FrameGenAnalysisTiming> timings_;
    ComPtr<ID3D12QueryHeap> capture_query_heap_;
    ComPtr<ID3D12Resource> capture_readback_;
    UINT64 capture_frequency_{0};
    UINT64 capture_calibration_gpu_{0};
    UINT64 capture_calibration_cpu_{0};
    std::array<std::pair<UINT64, std::chrono::steady_clock::time_point>, kCaptureSlots + kMotionSlots> capture_ends_{};
    std::size_t capture_end_next_{0};
    std::string dump_directory_; // DumpNextExecute
    // Research capture: the game's raw depth texels are copied at depth
    // capture while armed (each dumped frame arms each view's next one).
    std::string raw_depth_root_;
    std::uint32_t raw_depth_left_{0};
    // Engine motion vectors (CaptureMotion): the submissions in flight and
    // their outputs, the barrier policy, what was read back and not yet
    // taken, what the log should know, and counts for EngineStatus.
    std::array<MotionSlot, kMotionSlots> motion_slots_;
    std::vector<MotionView> motion_views_;
    MotionState motion_state_{MotionState::kAuto};
    ComPtr<ID3D12InfoQueue> info_queue_; // VECTORXR_FRAMEGEN_ENGINE_MV_DEBUG=1 under the debug layer
    UINT64 motion_messages_read_{0};
    std::uint64_t motion_count_{0};
    std::uint64_t motion_sequence_{0};
    std::deque<FrameGenEngineCensus> motion_census_;
    std::vector<std::string> motion_notes_;
    std::unordered_set<ID3D12Resource*> motion_described_;
    std::uint64_t motion_submitted_{0};
    std::uint64_t motion_busy_{0};
    std::uint64_t motion_refused_{0};
    std::uint64_t motion_failed_{0};
    std::uint64_t motion_dumped_{0};
    bool motion_stopped_{false};
    std::string motion_dump_directory_; // DumpMotion
    bool motion_dump_all_{false};
    // Stage 2: each view's raw fields (chosen with the first view's: three
    // from Observe, else two; VECTORXR_FRAMEGEN_ENGINE_RAW_SLOTS sets them)
    // and their format (VECTORXR_FRAMEGEN_ENGINE_MV_RESEARCH=z: RGBA16F, the
    // vectors' b and a kept); counts for EngineStatus; and the last
    // submission's time, for the device-removed sentinel (DeviceRemovedReason).
    std::uint8_t engine_slots_{2};
    std::optional<std::uint8_t> engine_slots_switch_;
    bool engine_slots_chosen_{false};
    DXGI_FORMAT engine_format_{DXGI_FORMAT_R16G16_FLOAT};
    // Stage 3: the vectors' convention worked out from the evidence (read
    // with each frame slot's next Execute, and in WaitIdle), against the
    // backend's own clock; frames whose evidence was read; the disable
    // noted once.
    FrameGenEngineValidator engine_validator_;
    std::chrono::steady_clock::time_point engine_clock_{std::chrono::steady_clock::now()};
    std::uint64_t engine_evidence_frames_{0};
    bool engine_disable_noted_{false};
    std::uint64_t motion_captured_{0};
    std::uint64_t motion_dispatches_{0};
    std::uint64_t motion_waits_{0};
    std::uint64_t motion_unready_{0};
    std::uint64_t motion_paired_{0};
    std::optional<std::chrono::steady_clock::time_point> motion_submitted_at_;
    bool motion_removed_noted_{false};
    // The raw fields could not be made (EnsureView): engine capture stays
    // off for the session, Frame Generation runs on without it.
    bool engine_failed_{false};
    std::unordered_map<XrSwapchain, std::vector<ID3D12Resource*>> images_;
    std::vector<ViewResources> views_;
};

} // namespace

std::unique_ptr<FrameGenGpu> CreateFrameGenD3D12(const void* binding, std::string& error, void* runtime_queue) {
    const auto& d3d12 = *static_cast<const XrGraphicsBindingD3D12KHR*>(binding);
    if (!d3d12.device || !d3d12.queue || d3d12.queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) {
        error = "Invalid D3D12 device or direct queue";
        return nullptr;
    }
    auto gpu = std::make_unique<D3D12FrameGen>(d3d12, static_cast<ID3D12CommandQueue*>(runtime_queue));
    if (!gpu->Initialize(error)) {
        return nullptr;
    }
    return gpu;
}

} // namespace depthxr
