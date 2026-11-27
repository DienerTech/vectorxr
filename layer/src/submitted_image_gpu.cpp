#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <openxr/openxr_platform.h>

#include "depthxr/d3d11_predication.h"
#include "depthxr/submitted_image.h"
#include "submitted_image_mirror.h"
#include "submitted_image_vulkan.h"

#include <algorithm>
#include <unordered_map>

namespace depthxr {
using Microsoft::WRL::ComPtr;
namespace {

std::string Failure(const char* operation, HRESULT result) {
    char code[16];
    sprintf_s(code, "0x%08lX", static_cast<unsigned long>(result));
    return std::string(operation) + " failed (" + code + ")";
}

// What both Direct3D backends check of a view before copying from its image.
bool CheckView(const SubmittedView& view, const SubmittedFormat& format, std::int64_t resource_format,
               std::uint64_t width, std::uint32_t height, std::uint32_t array_size, std::string& error) {
    if (format.kind == SubmittedPixelKind::kUnknown) {
        error = "format " + std::to_string(view.format) + " is not one that can be read";
        return false;
    }
    if (!SameDxgiFamily(resource_format, format.view_format)) {
        error = "the image's format " + std::to_string(resource_format) + " is not of its swapchain's format " +
                std::to_string(view.format);
        return false;
    }
    if (view.array_slice >= array_size) {
        error = "array slice " + std::to_string(view.array_slice) + " is outside the image's " +
                std::to_string(array_size);
        return false;
    }
    const XrRect2Di& rect = view.rect;
    if (rect.offset.x < 0 || rect.offset.y < 0 || rect.extent.width <= 0 || rect.extent.height <= 0 ||
        static_cast<std::uint64_t>(rect.offset.x) + static_cast<std::uint64_t>(rect.extent.width) > width ||
        static_cast<std::uint64_t>(rect.offset.y) + static_cast<std::uint64_t>(rect.extent.height) > height) {
        error = "the rectangle is empty or outside the image";
        return false;
    }
    return true;
}

// Direct3D 11: a staging texture per ticket, an event query to learn without
// a flush when the copy is done, and a map that does not wait.
class D3D11Images final : public SubmittedImageGpu {
    struct Ticket {
        ComPtr<ID3D11Texture2D> staging;
        ComPtr<ID3D11Query> done;
        SubmittedFormat format;
        std::uint32_t width{0};
        std::uint32_t height{0};
        std::uint32_t polls{0};
        bool mapped{false};
        D3D11_MAPPED_SUBRESOURCE map{};
    };
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    std::unordered_map<XrSwapchain, std::vector<ComPtr<ID3D11Texture2D>>> swapchains_;
    std::unordered_map<std::uint64_t, Ticket> tickets_;
    std::uint64_t next_ticket_{1};
    std::unique_ptr<D3D11Mirror> mirror_;
    bool context_states_;
    // What a quad views eye is drawn into with its focus view for a
    // screenshot (ReadbackComposed): kept while a picture fits in it, let go
    // of with the session.
    ComPtr<ID3D11Texture2D> composed_;
    std::uint32_t composed_width_{0};
    std::uint32_t composed_height_{0};

    // The focus view's image of a draw, where it is known: null without one.
    ID3D11Texture2D* FocusImage(const SubmittedFocus& focus) const {
        if (!focus.present) return nullptr;
        const auto found = swapchains_.find(focus.view.swapchain);
        if (found == swapchains_.end() || focus.view.image_index >= found->second.size()) return nullptr;
        return found->second[focus.view.image_index].Get();
    }

    // A ticket for `box` of the image's subresource, copied to a staging
    // texture of `copy_format`. `samples` > 1: resolved first.
    std::uint64_t Copy(ID3D11Texture2D* source, UINT subresource, const D3D11_BOX& box, const SubmittedFormat& format,
                       DXGI_FORMAT copy_format, const D3D11_TEXTURE2D_DESC& desc, std::string& error) {
        Ticket ticket;
        ticket.format = format;
        ticket.width = box.right - box.left;
        ticket.height = box.bottom - box.top;
        D3D11_TEXTURE2D_DESC staging{};
        staging.Width = ticket.width;
        staging.Height = ticket.height;
        staging.MipLevels = 1;
        staging.ArraySize = 1;
        staging.Format = copy_format;
        staging.SampleDesc.Count = 1;
        staging.Usage = D3D11_USAGE_STAGING;
        staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        HRESULT result = device_->CreateTexture2D(&staging, nullptr, &ticket.staging);
        if (FAILED(result)) {
            error = Failure("Creating the staging texture", result);
            return 0;
        }
        D3D11_QUERY_DESC query{D3D11_QUERY_EVENT, 0};
        result = device_->CreateQuery(&query, &ticket.done);
        if (FAILED(result)) {
            error = Failure("Creating the event query", result);
            return 0;
        }
        const ScopedD3D11Unpredicated unpredicated(context_.Get());
        if (desc.SampleDesc.Count > 1) {
            // Resolved whole into a single-sample image first: a resolve has
            // no rectangle.
            D3D11_TEXTURE2D_DESC resolved{};
            resolved.Width = desc.Width;
            resolved.Height = desc.Height;
            resolved.MipLevels = 1;
            resolved.ArraySize = 1;
            resolved.Format = copy_format;
            resolved.SampleDesc.Count = 1;
            resolved.Usage = D3D11_USAGE_DEFAULT;
            resolved.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            ComPtr<ID3D11Texture2D> single;
            result = device_->CreateTexture2D(&resolved, nullptr, &single);
            if (FAILED(result)) {
                error = Failure("Creating the resolve texture", result);
                return 0;
            }
            context_->ResolveSubresource(single.Get(), 0, source, subresource, copy_format);
            context_->CopySubresourceRegion(ticket.staging.Get(), 0, 0, 0, 0, single.Get(), 0, &box);
        } else {
            context_->CopySubresourceRegion(ticket.staging.Get(), 0, 0, 0, 0, source, subresource, &box);
        }
        context_->End(ticket.done.Get());
        const std::uint64_t id = next_ticket_++;
        tickets_.emplace(id, std::move(ticket));
        return id;
    }

    void ForgetViews(XrSwapchain swapchain) {
        const auto found = swapchains_.find(swapchain);
        if (found == swapchains_.end()) return;
        for (const ComPtr<ID3D11Texture2D>& texture : found->second) {
            if (mirror_) mirror_->Forget(texture.Get());
        }
    }

  public:
    explicit D3D11Images(ID3D11Device* device, bool context_states = true)
        : device_(device), context_states_(context_states) {
        device->GetImmediateContext(&context_);
    }
    ~D3D11Images() override { Reset(); }
    const char* Name() const override { return "D3D11"; }
    SubmittedImageApi Api() const override { return SubmittedImageApi::kD3D11; }

    bool Track(XrSwapchain swapchain, const XrSwapchainImageBaseHeader* images, std::uint32_t count,
               std::string& error) override {
        if (!images || count == 0 || images->type != XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR) {
            error = "not Direct3D 11 swapchain images";
            return false;
        }
        const auto* d3d11 = reinterpret_cast<const XrSwapchainImageD3D11KHR*>(images);
        std::vector<ComPtr<ID3D11Texture2D>> textures;
        for (std::uint32_t i = 0; i < count; ++i) {
            if (d3d11[i].type != XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR || !d3d11[i].texture) {
                error = "a swapchain image has no texture";
                return false;
            }
            textures.emplace_back(d3d11[i].texture);
        }
        swapchains_[swapchain] = std::move(textures);
        return true;
    }
    void Forget(XrSwapchain swapchain) override {
        ForgetViews(swapchain);
        swapchains_.erase(swapchain);
    }
    bool SupportsMirror() const override { return true; }
    MirrorDrawResult DrawMirror(const SubmittedView& view, const MirrorDraw& draw, std::string& error) override {
        const auto source = swapchains_.find(view.swapchain);
        if (source == swapchains_.end() || view.image_index >= source->second.size()) {
            error = "the swapchain's images are not known";
            return MirrorDrawResult::kFailed;
        }
        ID3D11Texture2D* target = nullptr;
        if (!draw.window) {
            const auto drawn = swapchains_.find(draw.target);
            if (drawn == swapchains_.end() || draw.target_image >= drawn->second.size()) {
                error = "no window, and the target's images are not known";
                return MirrorDrawResult::kFailed;
            }
            target = drawn->second[draw.target_image].Get();
        }
        if (!mirror_) mirror_ = std::make_unique<D3D11Mirror>(device_.Get(), context_.Get(), context_states_);
        const MirrorDrawResult prepared = mirror_->Prepare(draw, error);
        if (prepared != MirrorDrawResult::kShown) return prepared;
        const bool drawn = mirror_->Draw(source->second[view.image_index].Get(), view,
                                         ClassifyFormat(SubmittedImageApi::kD3D11, view.format), target, draw, error,
                                         FocusImage(draw.focus));
        if (!drawn) return MirrorDrawResult::kFailed;
        return target ? MirrorDrawResult::kShown : mirror_->Present(error);
    }
    void CloseMirror() override {
        if (mirror_) mirror_->Close();
    }
    bool Tracked(XrSwapchain swapchain) const override { return swapchains_.count(swapchain) != 0; }

    std::uint64_t Readback(const SubmittedView& view, std::string& error) override {
        const auto found = swapchains_.find(view.swapchain);
        if (found == swapchains_.end() || view.image_index >= found->second.size()) {
            error = "the swapchain's images are not known";
            return 0;
        }
        ID3D11Texture2D* source = found->second[view.image_index].Get();
        D3D11_TEXTURE2D_DESC desc{};
        source->GetDesc(&desc);
        const SubmittedFormat format = ClassifyFormat(SubmittedImageApi::kD3D11, view.format);
        if (!CheckView(view, format, desc.Format, desc.Width, desc.Height, desc.ArraySize, error)) {
            return 0;
        }
        const D3D11_BOX box{static_cast<UINT>(view.rect.offset.x),
                            static_cast<UINT>(view.rect.offset.y),
                            0,
                            static_cast<UINT>(view.rect.offset.x + view.rect.extent.width),
                            static_cast<UINT>(view.rect.offset.y + view.rect.extent.height),
                            1};
        return Copy(source, D3D11CalcSubresource(0, view.array_slice, desc.MipLevels), box, format,
                    static_cast<DXGI_FORMAT>(format.view_format), desc, error);
    }

    bool SupportsFocus() const override { return true; }
    std::uint64_t ReadbackComposed(const SubmittedView& view, const SubmittedFocus& focus, std::uint32_t width,
                                   std::uint32_t height, std::string& error) override {
        const auto found = swapchains_.find(view.swapchain);
        ID3D11Texture2D* const focus_image = FocusImage(focus);
        if (found == swapchains_.end() || view.image_index >= found->second.size() || !focus_image) {
            error = "the swapchain's images are not known";
            return 0;
        }
        if (width == 0 || height == 0 || width > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION ||
            height > D3D11_REQ_TEXTURE2D_U_OR_V_DIMENSION) {
            error = "a picture of " + std::to_string(width) + "x" + std::to_string(height) + " cannot be drawn";
            return 0;
        }
        constexpr DXGI_FORMAT kComposedFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
        if (!composed_ || composed_width_ < width || composed_height_ < height) {
            composed_.Reset();
            D3D11_TEXTURE2D_DESC desc{};
            desc.Width = width;
            desc.Height = height;
            desc.MipLevels = 1;
            desc.ArraySize = 1;
            desc.Format = kComposedFormat;
            desc.SampleDesc.Count = 1;
            desc.Usage = D3D11_USAGE_DEFAULT;
            desc.BindFlags = D3D11_BIND_RENDER_TARGET;
            const HRESULT result = device_->CreateTexture2D(&desc, nullptr, &composed_);
            if (FAILED(result)) {
                composed_.Reset();
                error = Failure("Creating the image a quad views eye is drawn into", result);
                return 0;
            }
            composed_width_ = width;
            composed_height_ = height;
        }
        MirrorDraw draw;
        draw.width = width;
        draw.height = height;
        draw.homography = MirrorWholeView();
        draw.focus = focus;
        if (!mirror_) mirror_ = std::make_unique<D3D11Mirror>(device_.Get(), context_.Get(), context_states_);
        const bool drawn = mirror_->Draw(found->second[view.image_index].Get(), view,
                                         ClassifyFormat(SubmittedImageApi::kD3D11, view.format), composed_.Get(), draw,
                                         error, focus_image);
        if (!drawn) return 0;
        D3D11_TEXTURE2D_DESC desc{};
        composed_->GetDesc(&desc);
        const D3D11_BOX box{0, 0, 0, width, height, 1};
        return Copy(composed_.Get(), 0, box, ClassifyFormat(SubmittedImageApi::kD3D11, kComposedFormat),
                    kComposedFormat, desc, error);
    }

    ReadbackState Poll(std::uint64_t id, SubmittedPixels& pixels, std::string& error) override {
        const auto found = tickets_.find(id);
        if (found == tickets_.end()) return ReadbackState::kNone;
        Ticket& ticket = found->second;
        if (!ticket.mapped) {
            // Without a flush while the game's own frames flush the context;
            // a context nothing flushes gets one after a few frames.
            const UINT flags = ++ticket.polls > 4 ? 0 : D3D11_ASYNC_GETDATA_DONOTFLUSH;
            BOOL done = FALSE;
            HRESULT result = context_->GetData(ticket.done.Get(), &done, sizeof(done), flags);
            if (FAILED(result)) {
                error = Failure("Reading the event query", result);
                return ReadbackState::kFailed;
            }
            if (result != S_OK || !done) return ReadbackState::kPending;
            result = context_->Map(ticket.staging.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &ticket.map);
            if (result == DXGI_ERROR_WAS_STILL_DRAWING) return ReadbackState::kPending;
            if (FAILED(result)) {
                error = Failure("Mapping the staging texture", result);
                return ReadbackState::kFailed;
            }
            ticket.mapped = true;
        }
        pixels.data = static_cast<const std::uint8_t*>(ticket.map.pData);
        pixels.row_pitch = ticket.map.RowPitch;
        pixels.width = ticket.width;
        pixels.height = ticket.height;
        pixels.format = ticket.format;
        return ReadbackState::kReady;
    }

    void Finish(std::uint64_t id) override {
        const auto found = tickets_.find(id);
        if (found == tickets_.end()) return;
        if (found->second.mapped) context_->Unmap(found->second.staging.Get(), 0);
        tickets_.erase(found);
        // With no readback left, the image quad views eyes were drawn into
        // for them goes too (the size of an eye, in video memory).
        if (tickets_.empty()) {
            composed_.Reset();
            composed_width_ = 0;
            composed_height_ = 0;
        }
    }

    void Reset() override {
        if (mirror_) mirror_->Reset();
        mirror_.reset();
        composed_.Reset();
        composed_width_ = 0;
        composed_height_ = 0;
        for (auto& [id, ticket] : tickets_) {
            if (ticket.mapped) context_->Unmap(ticket.staging.Get(), 0);
        }
        tickets_.clear();
        swapchains_.clear();
    }
};

// Direct3D 12: a command list per ticket on the session's queue, copying into
// a readback buffer, and a fence to learn when it is done. Released color
// images rest in RENDER_TARGET (the OpenXR Direct3D 12 convention, as the
// OSD's backend assumes) and are returned to it.
class D3D12Images final : public SubmittedImageGpu {
    struct Ticket {
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> commands;
        ComPtr<ID3D12Resource> buffer;
        ComPtr<ID3D12Resource> resolved;
        ComPtr<ID3D12Resource> source; // kept alive until the copy is done
        SubmittedFormat format;
        std::uint32_t width{0};
        std::uint32_t height{0};
        std::uint32_t row_pitch{0};
        std::uint64_t bytes{0};
        std::uint64_t serial{0};
        void* mapped{nullptr};
        bool finished{false}; // Finish came before the GPU was done: freed once it is
    };
    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<ID3D12Fence> fence_;
    std::unordered_map<XrSwapchain, std::vector<ComPtr<ID3D12Resource>>> swapchains_;
    std::unordered_map<std::uint64_t, Ticket> tickets_;
    std::uint64_t next_ticket_{1};
    std::uint64_t serial_{0};
    std::unique_ptr<D3D12Mirror> mirror_;
    // The mirror's sources are images shared with a Vulkan game's device
    // (resting in COMMON), not a runtime's swapchain images.
    bool shared_sources_{false};

    // A mirror draw the GPU has not run yet may
    // read the images.
    void RetireImages(XrSwapchain swapchain) {
        const auto found = swapchains_.find(swapchain);
        if (found == swapchains_.end()) return;
        for (const ComPtr<ID3D12Resource>& texture : found->second) {
            if (mirror_) mirror_->Retire(texture);
        }
    }

    static D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* resource, UINT subresource, D3D12_RESOURCE_STATES before,
                                             D3D12_RESOURCE_STATES after) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = resource;
        barrier.Transition.Subresource = subresource;
        barrier.Transition.StateBefore = before;
        barrier.Transition.StateAfter = after;
        return barrier;
    }
    // Tickets finished before their copies were: freed once the GPU is done.
    void Sweep() {
        if (!fence_) return;
        const std::uint64_t completed = fence_->GetCompletedValue();
        for (auto it = tickets_.begin(); it != tickets_.end();) {
            if (it->second.finished && it->second.serial <= completed) {
                it = tickets_.erase(it);
            } else {
                ++it;
            }
        }
    }

  public:
    explicit D3D12Images(const XrGraphicsBindingD3D12KHR& binding, bool shared_sources = false)
        : device_(binding.device), queue_(binding.queue), shared_sources_(shared_sources) {}
    ~D3D12Images() override { Reset(); }
    const char* Name() const override { return "D3D12"; }
    SubmittedImageApi Api() const override { return SubmittedImageApi::kD3D12; }

    bool Track(XrSwapchain swapchain, const XrSwapchainImageBaseHeader* images, std::uint32_t count,
               std::string& error) override {
        if (!images || count == 0 || images->type != XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR) {
            error = "not Direct3D 12 swapchain images";
            return false;
        }
        const auto* d3d12 = reinterpret_cast<const XrSwapchainImageD3D12KHR*>(images);
        std::vector<ComPtr<ID3D12Resource>> textures;
        for (std::uint32_t i = 0; i < count; ++i) {
            if (d3d12[i].type != XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR || !d3d12[i].texture) {
                error = "a swapchain image has no texture";
                return false;
            }
            textures.emplace_back(d3d12[i].texture);
        }
        swapchains_[swapchain] = std::move(textures);
        return true;
    }
    void Forget(XrSwapchain swapchain) override {
        RetireImages(swapchain);
        swapchains_.erase(swapchain);
    }
    bool SupportsMirror() const override { return true; }
    MirrorDrawResult DrawMirror(const SubmittedView& view, const MirrorDraw& draw, std::string& error) override {
        const auto source = swapchains_.find(view.swapchain);
        if (source == swapchains_.end() || view.image_index >= source->second.size()) {
            error = "the swapchain's images are not known";
            return MirrorDrawResult::kFailed;
        }
        ID3D12Resource* target = nullptr;
        if (!draw.window) {
            const auto drawn = swapchains_.find(draw.target);
            if (drawn == swapchains_.end() || draw.target_image >= drawn->second.size()) {
                error = "no window, and the target's images are not known";
                return MirrorDrawResult::kFailed;
            }
            target = drawn->second[draw.target_image].Get();
        }
        if (!mirror_) mirror_ = std::make_unique<D3D12Mirror>(device_.Get(), queue_.Get());
        return mirror_->Draw(source->second[view.image_index].Get(), view,
                             ClassifyFormat(SubmittedImageApi::kD3D12, view.format), target, draw, error,
                             shared_sources_ ? D3D12_RESOURCE_STATE_COMMON : D3D12_RESOURCE_STATE_RENDER_TARGET);
    }
    void CloseMirror() override {
        if (mirror_) mirror_->Close();
    }
    bool Tracked(XrSwapchain swapchain) const override { return swapchains_.count(swapchain) != 0; }

    std::uint64_t Readback(const SubmittedView& view, std::string& error) override {
        Sweep();
        const auto found = swapchains_.find(view.swapchain);
        if (found == swapchains_.end() || view.image_index >= found->second.size()) {
            error = "the swapchain's images are not known";
            return 0;
        }
        ID3D12Resource* source = found->second[view.image_index].Get();
        const D3D12_RESOURCE_DESC desc = source->GetDesc();
        const SubmittedFormat format = ClassifyFormat(SubmittedImageApi::kD3D12, view.format);
        if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
            error = "the image is not a 2D texture";
            return 0;
        }
        if (!CheckView(view, format, desc.Format, desc.Width, desc.Height, desc.DepthOrArraySize, error)) {
            return 0;
        }
        HRESULT result = device_->GetDeviceRemovedReason();
        if (FAILED(result)) {
            error = Failure("The Direct3D 12 device", result);
            return 0;
        }
        if (!fence_) {
            result = device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_));
            if (FAILED(result)) {
                error = Failure("Creating the fence", result);
                return 0;
            }
        }
        const auto copy_format = static_cast<DXGI_FORMAT>(format.view_format);
        const UINT mips = std::max<UINT>(desc.MipLevels, 1);
        const UINT subresource = view.array_slice * mips;
        Ticket ticket;
        ticket.format = format;
        ticket.width = static_cast<std::uint32_t>(view.rect.extent.width);
        ticket.height = static_cast<std::uint32_t>(view.rect.extent.height);
        ticket.source = source;

        // The readback buffer's layout for a width x height image of the format.
        D3D12_RESOURCE_DESC region{};
        region.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        region.Width = ticket.width;
        region.Height = ticket.height;
        region.DepthOrArraySize = 1;
        region.MipLevels = 1;
        region.Format = copy_format;
        region.SampleDesc.Count = 1;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT64 bytes = 0;
        device_->GetCopyableFootprints(&region, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
        if (bytes == 0 || bytes == UINT64_MAX) {
            error = "the image's copy layout is not known";
            return 0;
        }
        ticket.row_pitch = footprint.Footprint.RowPitch;
        ticket.bytes = bytes;
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC buffer{};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = bytes;
        buffer.Height = 1;
        buffer.DepthOrArraySize = 1;
        buffer.MipLevels = 1;
        buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        result = device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST,
                                                  nullptr, IID_PPV_ARGS(&ticket.buffer));
        if (FAILED(result)) {
            error = Failure("Creating the readback buffer", result);
            return 0;
        }
        result = device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&ticket.allocator));
        if (SUCCEEDED(result)) {
            result = device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, ticket.allocator.Get(), nullptr,
                                                IID_PPV_ARGS(&ticket.commands));
        }
        if (FAILED(result)) {
            error = Failure("Creating the command list", result);
            return 0;
        }
        ID3D12GraphicsCommandList* commands = ticket.commands.Get();
        D3D12_TEXTURE_COPY_LOCATION from{};
        from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION to{};
        to.pResource = ticket.buffer.Get();
        to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        to.PlacedFootprint = footprint;
        const D3D12_BOX box{static_cast<UINT>(view.rect.offset.x),
                            static_cast<UINT>(view.rect.offset.y),
                            0,
                            static_cast<UINT>(view.rect.offset.x + view.rect.extent.width),
                            static_cast<UINT>(view.rect.offset.y + view.rect.extent.height),
                            1};
        if (desc.SampleDesc.Count > 1) {
            D3D12_RESOURCE_DESC single = desc;
            single.DepthOrArraySize = 1;
            single.MipLevels = 1;
            single.Format = copy_format;
            single.SampleDesc = {1, 0};
            single.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            single.Alignment = 0;
            D3D12_HEAP_PROPERTIES plain{};
            plain.Type = D3D12_HEAP_TYPE_DEFAULT;
            result = device_->CreateCommittedResource(&plain, D3D12_HEAP_FLAG_NONE, &single,
                                                      D3D12_RESOURCE_STATE_RESOLVE_DEST, nullptr,
                                                      IID_PPV_ARGS(&ticket.resolved));
            if (FAILED(result)) {
                error = Failure("Creating the resolve texture", result);
                return 0;
            }
            D3D12_RESOURCE_BARRIER barrier = Transition(source, subresource, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                        D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
            commands->ResourceBarrier(1, &barrier);
            commands->ResolveSubresource(ticket.resolved.Get(), 0, source, subresource, copy_format);
            std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
            commands->ResourceBarrier(1, &barrier);
            barrier = Transition(ticket.resolved.Get(), 0, D3D12_RESOURCE_STATE_RESOLVE_DEST,
                                 D3D12_RESOURCE_STATE_COPY_SOURCE);
            commands->ResourceBarrier(1, &barrier);
            from.pResource = ticket.resolved.Get();
            from.SubresourceIndex = 0;
            commands->CopyTextureRegion(&to, 0, 0, 0, &from, &box);
        } else {
            D3D12_RESOURCE_BARRIER barrier = Transition(source, subresource, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                        D3D12_RESOURCE_STATE_COPY_SOURCE);
            commands->ResourceBarrier(1, &barrier);
            from.pResource = source;
            from.SubresourceIndex = subresource;
            commands->CopyTextureRegion(&to, 0, 0, 0, &from, &box);
            std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
            commands->ResourceBarrier(1, &barrier);
        }
        result = commands->Close();
        if (FAILED(result)) {
            error = Failure("Closing the command list", result);
            return 0;
        }
        ID3D12CommandList* lists[] = {commands};
        queue_->ExecuteCommandLists(1, lists);
        ticket.serial = ++serial_;
        result = queue_->Signal(fence_.Get(), ticket.serial);
        const std::uint64_t id = next_ticket_++;
        if (FAILED(result)) {
            // Executed with no way to learn when: its resources are kept
            // for the session (no fence value ever reaches its serial).
            error = Failure("Signalling the fence", result);
            ticket.finished = true;
            ticket.serial = UINT64_MAX;
            tickets_.emplace(id, std::move(ticket));
            return 0;
        }
        tickets_.emplace(id, std::move(ticket));
        return id;
    }

    ReadbackState Poll(std::uint64_t id, SubmittedPixels& pixels, std::string& error) override {
        const auto found = tickets_.find(id);
        if (found == tickets_.end() || found->second.finished) return ReadbackState::kNone;
        Ticket& ticket = found->second;
        if (!ticket.mapped) {
            if (fence_->GetCompletedValue() < ticket.serial) {
                const HRESULT removed = device_->GetDeviceRemovedReason();
                if (FAILED(removed)) {
                    error = Failure("The Direct3D 12 device", removed);
                    return ReadbackState::kFailed;
                }
                return ReadbackState::kPending;
            }
            const D3D12_RANGE range{0, static_cast<SIZE_T>(ticket.bytes)};
            const HRESULT result = ticket.buffer->Map(0, &range, &ticket.mapped);
            if (FAILED(result)) {
                ticket.mapped = nullptr;
                error = Failure("Mapping the readback buffer", result);
                return ReadbackState::kFailed;
            }
        }
        pixels.data = static_cast<const std::uint8_t*>(ticket.mapped);
        pixels.row_pitch = ticket.row_pitch;
        pixels.width = ticket.width;
        pixels.height = ticket.height;
        pixels.format = ticket.format;
        return ReadbackState::kReady;
    }

    void Finish(std::uint64_t id) override {
        const auto found = tickets_.find(id);
        if (found == tickets_.end()) return;
        Ticket& ticket = found->second;
        if (ticket.mapped) {
            const D3D12_RANGE none{0, 0};
            ticket.buffer->Unmap(0, &none);
            ticket.mapped = nullptr;
        }
        ticket.finished = true;
        Sweep();
    }

    void Reset() override {
        if (mirror_) mirror_->Reset();
        mirror_.reset();
        // The GPU may still run a ticket's commands: wait for the last of
        // them on a live device before their memory goes.
        std::uint64_t last = 0;
        for (auto& [id, ticket] : tickets_) {
            if (ticket.mapped) {
                const D3D12_RANGE none{0, 0};
                ticket.buffer->Unmap(0, &none);
                ticket.mapped = nullptr;
            }
            if (ticket.serial != UINT64_MAX) last = std::max(last, ticket.serial);
        }
        if (fence_ && last && fence_->GetCompletedValue() < last && SUCCEEDED(device_->GetDeviceRemovedReason())) {
            if (HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr)) {
                if (SUCCEEDED(fence_->SetEventOnCompletion(last, event))) WaitForSingleObject(event, 5000);
                CloseHandle(event);
            }
        }
        const bool done = !fence_ || fence_->GetCompletedValue() >= last || FAILED(device_->GetDeviceRemovedReason());
        for (auto& [id, ticket] : tickets_) {
            if (!done || ticket.serial == UINT64_MAX) {
                // Never freed under the GPU: left to the device.
                ticket.allocator.Detach();
                ticket.commands.Detach();
                ticket.buffer.Detach();
                ticket.resolved.Detach();
                ticket.source.Detach();
            }
        }
        tickets_.clear();
        swapchains_.clear();
    }
};

} // namespace

std::unique_ptr<SubmittedImageGpu> CreateSubmittedImageD3D11(ID3D11Device* device, bool context_states) {
    return device ? std::make_unique<D3D11Images>(device, context_states) : nullptr;
}

namespace {

// A Vulkan game's session on Frame Generation's Vulkan bridge: the runtime's
// images are Direct3D 12 ones on the bridge's device, and only the mirror
// window is drawn from them.
class MirrorOnlyImages final : public SubmittedImageGpu {
  public:
    explicit MirrorOnlyImages(std::unique_ptr<SubmittedImageGpu> images) : images_(std::move(images)) {}
    const char* Name() const override { return "Vulkan (on Frame Generation's Direct3D 12 bridge)"; }
    SubmittedImageApi Api() const override { return images_->Api(); }
    bool SupportsReadback() const override { return false; }
    bool Track(XrSwapchain swapchain, const XrSwapchainImageBaseHeader* images, std::uint32_t count,
               std::string& error) override {
        return images_->Track(swapchain, images, count, error);
    }
    void Forget(XrSwapchain swapchain) override { images_->Forget(swapchain); }
    bool Tracked(XrSwapchain swapchain) const override { return images_->Tracked(swapchain); }
    std::uint64_t Readback(const SubmittedView&, std::string& error) override {
        error = "Vulkan images cannot be read back yet";
        return 0;
    }
    ReadbackState Poll(std::uint64_t, SubmittedPixels&, std::string&) override { return ReadbackState::kNone; }
    void Finish(std::uint64_t) override {}
    bool SupportsMirror() const override { return images_->SupportsMirror(); }
    MirrorDrawResult DrawMirror(const SubmittedView& view, const MirrorDraw& draw, std::string& error) override {
        return images_->DrawMirror(view, draw, error);
    }
    void CloseMirror() override { images_->CloseMirror(); }
    void SetQueueMutex(std::mutex* mutex) override { images_->SetQueueMutex(mutex); }
    void Abandon() override { images_->Abandon(); }
    void Reset() override { images_->Reset(); }

  private:
    std::unique_ptr<SubmittedImageGpu> images_;
};

} // namespace

std::unique_ptr<SubmittedImageGpu> CreateSubmittedImageMirrorOnly(const void* binding_chain, std::string& error) {
    SubmittedImageBinding captured = CaptureSubmittedImageBinding(binding_chain);
    if (captured.type != XR_TYPE_GRAPHICS_BINDING_D3D12_KHR) {
        error = "The Vulkan bridge has no Direct3D 12 binding";
        return nullptr;
    }
    XrGraphicsBindingD3D12KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};
    binding.device = static_cast<ID3D12Device*>(captured.device);
    binding.queue = static_cast<ID3D12CommandQueue*>(captured.queue);
    if (!binding.device || !binding.queue || binding.queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) {
        error = "The Vulkan bridge's Direct3D 12 binding has no device or direct queue";
        return nullptr;
    }
    return std::make_unique<MirrorOnlyImages>(std::make_unique<D3D12Images>(binding, true));
}

bool TrackSubmittedSwapchain(
    SubmittedImageGpu& gpu, XrSwapchain swapchain,
    const std::function<XrResult(XrSwapchain, std::uint32_t, std::uint32_t*, XrSwapchainImageBaseHeader*)>& enumerate,
    std::string& error) {
    if (!enumerate) {
        error = "xrEnumerateSwapchainImages is not available";
        return false;
    }
    std::uint32_t count = 0;
    XrResult result = enumerate(swapchain, 0, &count, nullptr);
    if (XR_FAILED(result) || count == 0) {
        error = "xrEnumerateSwapchainImages failed (" + std::to_string(static_cast<int>(result)) + ")";
        return false;
    }
    const auto track = [&](auto& images) {
        result = enumerate(swapchain, count, &count, reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data()));
        if (XR_FAILED(result)) {
            error = "xrEnumerateSwapchainImages failed (" + std::to_string(static_cast<int>(result)) + ")";
            return false;
        }
        return gpu.Track(swapchain, reinterpret_cast<const XrSwapchainImageBaseHeader*>(images.data()), count, error);
    };
    switch (gpu.Api()) {
    case SubmittedImageApi::kD3D11: {
        std::vector<XrSwapchainImageD3D11KHR> images(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
        return track(images);
    }
    case SubmittedImageApi::kD3D12: {
        std::vector<XrSwapchainImageD3D12KHR> images(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
        return track(images);
    }
    case SubmittedImageApi::kVulkan:
        return TrackSubmittedSwapchainVulkan(gpu, swapchain, count, enumerate, error);
    default:
        error = "this graphics API's images cannot be tracked";
        return false;
    }
}

bool TrackSubmittedSwapchainD3D12(SubmittedImageGpu& gpu, XrSwapchain swapchain, void* const* resources,
                                  std::uint32_t count, std::string& error) {
    if (gpu.Api() != SubmittedImageApi::kD3D12 || !resources || count == 0) {
        error = "not Direct3D 12 swapchain images";
        return false;
    }
    std::vector<XrSwapchainImageD3D12KHR> images(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR});
    for (std::uint32_t i = 0; i < count; ++i) images[i].texture = static_cast<ID3D12Resource*>(resources[i]);
    return gpu.Track(swapchain, reinterpret_cast<const XrSwapchainImageBaseHeader*>(images.data()), count, error);
}

bool TrackSubmittedSwapchainD3D11(SubmittedImageGpu& gpu, XrSwapchain swapchain, void* const* textures,
                                  std::uint32_t count, std::string& error) {
    if (gpu.Api() != SubmittedImageApi::kD3D11 || !textures || count == 0) {
        error = "not Direct3D 11 swapchain images";
        return false;
    }
    std::vector<XrSwapchainImageD3D11KHR> images(count, {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
    for (std::uint32_t i = 0; i < count; ++i) images[i].texture = static_cast<ID3D11Texture2D*>(textures[i]);
    return gpu.Track(swapchain, reinterpret_cast<const XrSwapchainImageBaseHeader*>(images.data()), count, error);
}

SubmittedImageBinding CaptureSubmittedImageBinding(const void* binding_chain) {
    SubmittedImageBinding captured;
    for (auto* entry = static_cast<const XrBaseInStructure*>(binding_chain); entry; entry = entry->next) {
        if (entry->type == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR) {
            captured.type = entry->type;
            captured.device = reinterpret_cast<const XrGraphicsBindingD3D11KHR*>(entry)->device;
            break;
        }
        if (entry->type == XR_TYPE_GRAPHICS_BINDING_D3D12_KHR) {
            const auto& binding = *reinterpret_cast<const XrGraphicsBindingD3D12KHR*>(entry);
            captured.type = entry->type;
            captured.device = binding.device;
            captured.queue = binding.queue;
            break;
        }
        if (CaptureVulkanBinding(entry, captured)) {
            break;
        }
    }
    return captured;
}

std::unique_ptr<SubmittedImageGpu> CreateSubmittedImageGpu(const SubmittedImageBinding& captured, std::string& error) {
    if (captured.type == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR) {
        if (captured.device) return CreateSubmittedImageD3D11(static_cast<ID3D11Device*>(captured.device));
        error = "The Direct3D 11 binding has no device";
        return nullptr;
    }
    if (captured.type == XR_TYPE_GRAPHICS_BINDING_D3D12_KHR) {
        XrGraphicsBindingD3D12KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};
        binding.device = static_cast<ID3D12Device*>(captured.device);
        binding.queue = static_cast<ID3D12CommandQueue*>(captured.queue);
        if (binding.device && binding.queue && binding.queue->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
            return std::make_unique<D3D12Images>(binding);
        }
        error = "The Direct3D 12 binding has no device or direct queue";
        return nullptr;
    }
    if (captured.type == XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR) {
        return CreateSubmittedImageVulkan(captured, error);
    }
    error = "The session has no Direct3D 11 or Direct3D 12 graphics binding";
    return nullptr;
}

std::unique_ptr<SubmittedImageGpu> CreateSubmittedImageGpu(const void* binding_chain, std::string& error) {
    return CreateSubmittedImageGpu(CaptureSubmittedImageBinding(binding_chain), error);
}

} // namespace depthxr
