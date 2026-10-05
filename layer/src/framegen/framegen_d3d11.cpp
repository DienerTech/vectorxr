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

DXGI_FORMAT IntermediateFormat(FrameGenOutputKind kind) {
    return kind == FrameGenOutputKind::kFloat16 ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
}

DXGI_FORMAT PassOutputFormat(FrameGenTextureKind kind) {
    switch (kind) {
    case FrameGenTextureKind::kLumaNewest:
    case FrameGenTextureKind::kLumaPrevious:
        return DXGI_FORMAT_R32_FLOAT;
    default:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    }
}

struct Texture {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> srv;
    ComPtr<ID3D11UnorderedAccessView> uav;
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
    DXGI_FORMAT history_format{DXGI_FORMAT_UNKNOWN};
    std::array<Texture, 2> history;
    std::array<Texture, 2> filtered;
    std::array<Texture, kFrameGenPyramidLevels + 1> luma_newest;
    std::array<Texture, kFrameGenPyramidLevels + 1> luma_previous;
    std::array<Texture, kFrameGenPyramidLevels + 1> flow;
    Texture output;
};

class D3D11FrameGen final : public FrameGenGpu {
  public:
    explicit D3D11FrameGen(ID3D11Device* device) : device_(device) { device_->GetImmediateContext(&context_); }

    bool Initialize(std::string& error) {
        for (std::size_t pass = 0; pass < kFrameGenPassCount; ++pass) {
            const auto& blob = framegen_shaders::kDxbc[pass];
            const HRESULT hr = device_->CreateComputeShader(blob.data, blob.size, nullptr, &shaders_[pass]);
            if (FAILED(hr)) {
                error = Failure("D3D11 Frame Generation compute shader", hr);
                return false;
            }
        }
        D3D11_BUFFER_DESC constants{};
        constants.ByteWidth = sizeof(FrameGenConstants);
        constants.Usage = D3D11_USAGE_DEFAULT;
        constants.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        HRESULT hr = device_->CreateBuffer(&constants, nullptr, &constants_);
        if (FAILED(hr)) {
            error = Failure("D3D11 Frame Generation constants", hr);
            return false;
        }
        D3D11_SAMPLER_DESC sampler{};
        sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler.MaxLOD = D3D11_FLOAT32_MAX;
        hr = device_->CreateSamplerState(&sampler, &sampler_);
        if (FAILED(hr)) {
            error = Failure("D3D11 Frame Generation sampler", hr);
            return false;
        }
        for (auto& query : timing_) {
            D3D11_QUERY_DESC disjoint{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
            D3D11_QUERY_DESC stamp{D3D11_QUERY_TIMESTAMP, 0};
            if (FAILED(device_->CreateQuery(&disjoint, &query.disjoint)) ||
                FAILED(device_->CreateQuery(&stamp, &query.begin)) || FAILED(device_->CreateQuery(&stamp, &query.end))) {
                query = {};
            }
        }
        return true;
    }

    const char* Name() const override { return "D3D11"; }

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
        std::vector<XrSwapchainImageD3D11KHR> native(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
        result = enumerate(swapchain, count, &count, reinterpret_cast<XrSwapchainImageBaseHeader*>(native.data()));
        if (XR_FAILED(result)) {
            error = "xrEnumerateSwapchainImages result=" + std::to_string(result);
            return false;
        }
        auto& textures = images_[swapchain];
        for (const auto& image : native) {
            textures.push_back(image.texture);
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
        if (views_.size() <= index) {
            views_.resize(index + 1);
        }
        ViewResources& view = views_[index];
        ID3D11Texture2D* texture = it->second[source.image_index];
        D3D11_TEXTURE2D_DESC desc{};
        texture->GetDesc(&desc);
        // Capture never reallocates; Execute sets the view up first.
        if (!view.output.texture ||
            !view.history_state.Matches(request.extent, desc.Format, request.output, FrameGenFinalFlowLevel(quality))) {
            error.clear();
            return false;
        }
        SavedState saved;
        Save(saved);
        CopySource(view, source, texture);
        Restore(saved);
        view.capture = {true, source.swapchain, source.image_index, source.array_index, source.rect};
        return true;
    }

    // The immediate context keeps resources alive for queued work.
    bool WaitIdle() override { return true; }

    bool Execute(const std::vector<FrameGenViewSubmission>& views, const FrameGenTuning& requested,
                 FrameGenExecuteResult& result, std::string& error) override {
        // No optical flow hardware path: block matching measures motion.
        FrameGenTuning tuning = requested;
        tuning.hardware_flow = false;
        result.synthesized.assign(views.size(), false);
        if (views_.size() < views.size()) {
            views_.resize(views.size());
        }
        SavedState saved;
        Save(saved);
        TimingQuery* timing = BeginTiming();
        bool ok = true;
        for (std::size_t i = 0; i < views.size() && ok; ++i) {
            bool synthesized = false;
            ok = ExecuteView(views[i], tuning, views_[i], synthesized, error);
            result.synthesized[i] = synthesized;
        }
        EndTiming(timing);
        Restore(saved);
        if (ok) {
            const HRESULT removed = device_->GetDeviceRemovedReason();
            if (FAILED(removed)) {
                error = Failure("D3D11 device", removed);
                ok = false;
            }
        }
        return ok;
    }

    void ResetHistory() override {
        for (auto& view : views_) {
            view.history_state.Invalidate();
        }
    }

    void Reset() override {
        views_.clear();
        images_.clear();
    }

    double LastGpuMilliseconds() const override { return last_gpu_ms_; }

  private:
    struct SavedState {
        ComPtr<ID3D11ComputeShader> shader;
        std::array<ID3D11ShaderResourceView*, 4> srvs{};
        std::array<ID3D11UnorderedAccessView*, 2> uavs{};
        ID3D11Buffer* constants{};
        ID3D11SamplerState* sampler{};
    };

    struct TimingQuery {
        ComPtr<ID3D11Query> disjoint, begin, end;
        bool pending{false};
    };

    void Save(SavedState& saved) {
        context_->CSGetShader(&saved.shader, nullptr, nullptr);
        context_->CSGetShaderResources(0, 4, saved.srvs.data());
        context_->CSGetUnorderedAccessViews(0, 2, saved.uavs.data());
        context_->CSGetConstantBuffers(0, 1, &saved.constants);
        context_->CSGetSamplers(0, 1, &saved.sampler);
    }

    void Restore(SavedState& saved) {
        context_->CSSetShader(saved.shader.Get(), nullptr, 0);
        context_->CSSetShaderResources(0, 4, saved.srvs.data());
        // Counters are not preserved by CSGetUnorderedAccessViews; -1 keeps
        // the current hidden counter value for append/consume buffers.
        const UINT keep[2] = {static_cast<UINT>(-1), static_cast<UINT>(-1)};
        context_->CSSetUnorderedAccessViews(0, 2, saved.uavs.data(), keep);
        context_->CSSetConstantBuffers(0, 1, &saved.constants);
        context_->CSSetSamplers(0, 1, &saved.sampler);
        for (auto* srv : saved.srvs) {
            if (srv) srv->Release();
        }
        for (auto* uav : saved.uavs) {
            if (uav) uav->Release();
        }
        if (saved.constants) saved.constants->Release();
        if (saved.sampler) saved.sampler->Release();
    }

    TimingQuery* BeginTiming() {
        // Read the oldest completed query without stalling the pipeline.
        for (auto& query : timing_) {
            if (!query.pending || !query.disjoint) {
                continue;
            }
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
            UINT64 begin = 0, end = 0;
            if (context_->GetData(query.disjoint.Get(), &disjoint, sizeof(disjoint), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
                context_->GetData(query.begin.Get(), &begin, sizeof(begin), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
                context_->GetData(query.end.Get(), &end, sizeof(end), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK) {
                query.pending = false;
                if (!disjoint.Disjoint && disjoint.Frequency && end >= begin) {
                    last_gpu_ms_ = static_cast<double>(end - begin) * 1000.0 / static_cast<double>(disjoint.Frequency);
                }
            }
        }
        TimingQuery& query = timing_[timing_index_];
        if (!query.disjoint || query.pending) {
            return nullptr;
        }
        timing_index_ = (timing_index_ + 1) % timing_.size();
        context_->Begin(query.disjoint.Get());
        context_->End(query.begin.Get());
        return &query;
    }

    void EndTiming(TimingQuery* query) {
        if (!query) {
            return;
        }
        context_->End(query->end.Get());
        context_->End(query->disjoint.Get());
        query->pending = true;
    }

    bool Create(Texture& texture, FrameGenExtent extent, DXGI_FORMAT format, DXGI_FORMAT view_format, bool uav,
                std::string& error) {
        texture = {};
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = extent.width;
        desc.Height = extent.height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | (uav ? D3D11_BIND_UNORDERED_ACCESS : 0);
        HRESULT hr = device_->CreateTexture2D(&desc, nullptr, &texture.texture);
        if (SUCCEEDED(hr)) {
            D3D11_SHADER_RESOURCE_VIEW_DESC srv{};
            srv.Format = view_format;
            srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            srv.Texture2D.MipLevels = 1;
            hr = device_->CreateShaderResourceView(texture.texture.Get(), &srv, &texture.srv);
        }
        if (SUCCEEDED(hr) && uav) {
            D3D11_UNORDERED_ACCESS_VIEW_DESC desc_uav{};
            desc_uav.Format = format;
            desc_uav.ViewDimension = D3D11_UAV_DIMENSION_TEXTURE2D;
            hr = device_->CreateUnorderedAccessView(texture.texture.Get(), &desc_uav, &texture.uav);
        }
        if (FAILED(hr)) {
            error = Failure("D3D11 Frame Generation texture", hr);
            texture = {};
            return false;
        }
        return true;
    }

    bool EnsureView(ViewResources& view, const FrameGenViewSubmission& submission, ID3D11Texture2D* source,
                    const FrameGenTuning& tuning, std::string& error) {
        D3D11_TEXTURE2D_DESC source_desc{};
        source->GetDesc(&source_desc);
        const FrameGenExtent extent = submission.request.extent;
        const std::uint8_t final_level = FrameGenFinalFlowLevel(tuning.quality);
        const unsigned change = view.history_state.Configure(extent, source_desc.Format, submission.request.output,
                                                              final_level);
        const auto levels = FrameGenPyramid(extent);
        if (change & FrameGenHistoryState::kAll || !view.output.texture) {
            view.capture.valid = false;
            view.history_format = source_desc.Format;
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
            const DXGI_FORMAT intermediate = IntermediateFormat(submission.request.output);
            if (!Create(view.output, extent, intermediate, intermediate, true, error)) return false;
        }
        if (change != FrameGenHistoryState::kNone || !view.filtered[0].texture) {
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

    bool ExecuteView(const FrameGenViewSubmission& submission, const FrameGenTuning& tuning, ViewResources& view,
                     bool& synthesized, std::string& error) {
        const auto source_it = images_.find(submission.source.swapchain);
        if (source_it == images_.end() || submission.source.image_index >= source_it->second.size() ||
            submission.outputs.size() < tuning.generated) {
            error = "Frame Generation image is not tracked";
            return false;
        }
        std::vector<ID3D11Texture2D*> outputs;
        for (const FrameGenImage& image : submission.outputs) {
            const auto output_it = images_.find(image.swapchain);
            if (output_it == images_.end() || image.image_index >= output_it->second.size()) {
                error = "Frame Generation output image is not tracked";
                return false;
            }
            outputs.push_back(output_it->second[image.image_index]);
        }
        ID3D11Texture2D* source = source_it->second[submission.source.image_index];
        if (!EnsureView(view, submission, source, tuning, error)) {
            view.history_state.Invalidate();
            return false;
        }

        if (!view.capture.Matches(submission.source)) {
            CopySource(view, submission.source, source);
        }

        // No depth capture on this backend yet: head rotation only.
        FrameGenViewRequest effective = view.history_state.Effective(submission.request);
        effective.depth = false;
        const FrameGenViewPlan plan = BuildFrameGenViewPlan(effective, tuning);
        context_->CSSetConstantBuffers(0, 1, constants_.GetAddressOf());
        context_->CSSetSamplers(0, 1, sampler_.GetAddressOf());
        for (const FrameGenDispatch& dispatch : plan.dispatches) {
            std::array<ID3D11ShaderResourceView*, 4> srvs{};
            std::array<ID3D11UnorderedAccessView*, 2> uavs{};
            for (std::size_t i = 0; i < 4; ++i) {
                if (Texture* texture = Resolve(view, dispatch.inputs[i])) srvs[i] = texture->srv.Get();
            }
            for (std::size_t i = 0; i < 2; ++i) {
                if (Texture* texture = Resolve(view, dispatch.outputs[i])) uavs[i] = texture->uav.Get();
            }
            context_->UpdateSubresource(constants_.Get(), 0, nullptr, &dispatch.constants, 0, 0);
            context_->CSSetShader(shaders_[static_cast<std::size_t>(dispatch.pass)].Get(), nullptr, 0);
            context_->CSSetShaderResources(0, 4, srvs.data());
            context_->CSSetUnorderedAccessViews(0, 2, uavs.data(), nullptr);
            context_->Dispatch((dispatch.width + kFrameGenThreadGroupSize - 1) / kFrameGenThreadGroupSize,
                               (dispatch.height + kFrameGenThreadGroupSize - 1) / kFrameGenThreadGroupSize, 1);
            // Unbind so the next pass may read this pass's output.
            constexpr std::array<ID3D11ShaderResourceView*, 4> no_srvs{};
            constexpr std::array<ID3D11UnorderedAccessView*, 2> no_uavs{};
            context_->CSSetShaderResources(0, 4, no_srvs.data());
            context_->CSSetUnorderedAccessViews(0, 2, no_uavs.data(), nullptr);
            for (std::size_t index = 0; index < outputs.size(); ++index) {
                if (!(dispatch.copy_mask & (1u << index))) {
                    continue;
                }
                D3D11_TEXTURE2D_DESC output_desc{};
                outputs[index]->GetDesc(&output_desc);
                const FrameGenImage& image = submission.outputs[index];
                context_->CopySubresourceRegion(outputs[index], D3D11CalcSubresource(0, image.array_index, output_desc.MipLevels),
                                                static_cast<UINT>(image.rect.offset.x), static_cast<UINT>(image.rect.offset.y),
                                                0, view.output.texture.Get(), 0, nullptr);
            }
        }
        synthesized = plan.synthesize;
        view.history_state.Commit(plan.synthesize);
        view.capture.valid = false;
        return true;
    }

    void CopySource(ViewResources& view, const FrameGenImage& image, ID3D11Texture2D* source) {
        D3D11_TEXTURE2D_DESC source_desc{};
        source->GetDesc(&source_desc);
        const auto& rect = image.rect;
        const D3D11_BOX box{static_cast<UINT>(rect.offset.x), static_cast<UINT>(rect.offset.y), 0,
                            static_cast<UINT>(rect.offset.x + rect.extent.width),
                            static_cast<UINT>(rect.offset.y + rect.extent.height), 1};
        context_->CopySubresourceRegion(view.history[view.history_state.Newest()].texture.Get(), 0, 0, 0, 0, source,
                                        D3D11CalcSubresource(0, image.array_index, source_desc.MipLevels), &box);
    }

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    std::array<ComPtr<ID3D11ComputeShader>, kFrameGenPassCount> shaders_;
    ComPtr<ID3D11Buffer> constants_;
    ComPtr<ID3D11SamplerState> sampler_;
    std::unordered_map<XrSwapchain, std::vector<ID3D11Texture2D*>> images_;
    std::vector<ViewResources> views_;
    std::array<TimingQuery, 4> timing_{};
    std::size_t timing_index_{0};
    double last_gpu_ms_{-1.0};
};

} // namespace

std::unique_ptr<FrameGenGpu> CreateFrameGenD3D11(const void* binding, std::string& error) {
    const auto& d3d11 = *static_cast<const XrGraphicsBindingD3D11KHR*>(binding);
    if (!d3d11.device) {
        error = "Invalid D3D11 graphics binding";
        return nullptr;
    }
    if (d3d11.device->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_0) {
        error = "Frame Generation needs a feature level 11_0 Direct3D 11 device";
        return nullptr;
    }
    auto gpu = std::make_unique<D3D11FrameGen>(d3d11.device);
    if (!gpu->Initialize(error)) {
        return nullptr;
    }
    return gpu;
}

} // namespace depthxr
