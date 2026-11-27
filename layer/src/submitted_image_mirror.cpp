#include "submitted_image_mirror.h"

#include <algorithm>
#include <atomic>
#include <cstring>

#include "mirror_focus_ps.h"
#include "mirror_ps.h"
#include "mirror_vs.h"

namespace depthxr {
using Microsoft::WRL::ComPtr;
namespace {

std::string Failure(const char* operation, HRESULT result) {
    char code[16];
    sprintf_s(code, "0x%08lX", static_cast<unsigned long>(result));
    return std::string(operation) + " failed (" + code + ")";
}

// Texels of an sRGB format's view and of half-float images are linear; 8-bit
// and 10-bit UNORM images hold display-encoded values.
bool LinearTexels(const SubmittedFormat& format) {
    return format.srgb || format.kind == SubmittedPixelKind::kFloat16;
}

// What both backends check before a draw.
bool CheckDraw(const SubmittedView& view, const SubmittedFormat& source_format, std::int64_t source_resource_format,
               std::uint64_t source_width, std::uint32_t source_height, std::uint32_t source_array,
               std::uint32_t source_samples, const MirrorDraw& draw, std::string& error) {
    if (source_format.kind == SubmittedPixelKind::kUnknown) {
        error = "format " + std::to_string(view.format) + " is not one that can be shown";
        return false;
    }
    if (!SameDxgiFamily(source_resource_format, source_format.view_format)) {
        error = "the image's format is not of its swapchain's format";
        return false;
    }
    if (source_samples > 1) {
        error = "multisampled images are not shown";
        return false;
    }
    if (view.array_slice >= source_array) {
        error = "array slice " + std::to_string(view.array_slice) + " is outside the image's " +
                std::to_string(source_array);
        return false;
    }
    const XrRect2Di& rect = view.rect;
    if (rect.offset.x < 0 || rect.offset.y < 0 || rect.extent.width <= 0 || rect.extent.height <= 0 ||
        static_cast<std::uint64_t>(rect.offset.x) + static_cast<std::uint64_t>(rect.extent.width) > source_width ||
        static_cast<std::uint64_t>(rect.offset.y) + static_cast<std::uint64_t>(rect.extent.height) > source_height) {
        error = "the rectangle is empty or outside the image";
        return false;
    }
    if (draw.width == 0 || draw.height == 0) {
        error = "the window has no picture";
        return false;
    }
    return true;
}

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* resource, UINT subresource, D3D12_RESOURCE_STATES before,
                                  D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = subresource;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    return barrier;
}

constexpr UINT kSourceSlots = 16;

} // namespace

// ------------------------------------------------------------ the swap chain

namespace {
std::atomic<std::uint32_t> g_swapchain_delay_ms{0};
std::atomic<std::uint32_t> g_swapchains_made{0};
} // namespace

std::uint32_t MirrorSwapChainsMadeForTests() {
    return g_swapchains_made.load(std::memory_order_acquire);
}

void SetMirrorSwapChainDelayForTests(std::uint32_t milliseconds) {
    g_swapchain_delay_ms.store(milliseconds, std::memory_order_release);
}

// On the window's thread (or the caller's, when the window's cannot take it).
void MirrorSwapChain::Make(const std::shared_ptr<Making>& making, ComPtr<IDXGIFactory2> factory,
                           ComPtr<IUnknown> device_or_queue, HWND window, std::uint32_t width, std::uint32_t height,
                           ComPtr<IDXGISwapChain3> replaced, bool first_present) {
    // The window can have one flip-model swap chain: the old one first.
    replaced.Reset();
    if (const std::uint32_t delay = g_swapchain_delay_ms.load(std::memory_order_acquire)) Sleep(delay);
    {
        std::scoped_lock lock(making->mutex);
        if (making->abandoned) {
            making->busy = false;
            return;
        }
    }
    ComPtr<IDXGISwapChain3> swapchain;
    HANDLE waitable = nullptr;
    std::string error;
    HRESULT result = S_OK;
    if (!factory) {
        result = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
        if (FAILED(result)) error = Failure("Creating the DXGI factory", result);
    }
    BOOL tearing = FALSE;
    DXGI_SWAP_CHAIN_DESC1 desc{};
    if (SUCCEEDED(result)) {
        ComPtr<IDXGIFactory5> factory5;
        if (SUCCEEDED(factory.As(&factory5)) &&
            FAILED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing)))) {
            tearing = FALSE;
        }
        desc.Width = width;
        desc.Height = height;
        desc.Format = kFormat;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = kBuffers;
        // Of the picture's size, not the window's: Windows stretches the
        // picture to the window's client area when it presents, so the
        // window can be dragged and resized with this one swap chain and the
        // picture stays live meanwhile. (The window keeps the picture's
        // aspect itself; the flip model offers no aspect-keeping stretch to
        // a window's swap chain.)
        desc.Scaling = DXGI_SCALING_STRETCH;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        // Presented without waiting for the display, torn where the display
        // allows it; the waitable object says when a frame can be queued.
        desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT |
                     (tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0u);
        ComPtr<IDXGISwapChain1> created;
        result = factory->CreateSwapChainForHwnd(device_or_queue.Get(), window, &desc, nullptr, nullptr, &created);
        if (FAILED(result)) {
            desc.Flags = 0;
            tearing = FALSE;
            result = factory->CreateSwapChainForHwnd(device_or_queue.Get(), window, &desc, nullptr, nullptr, &created);
        }
        if (SUCCEEDED(result)) result = created.As(&swapchain);
        if (FAILED(result)) {
            swapchain.Reset();
            error = Failure("Creating the window's swap chain", result);
        } else {
            g_swapchains_made.fetch_add(1, std::memory_order_acq_rel);
            // Never full screen, and no Alt+Enter of DXGI's on the window.
            factory->MakeWindowAssociation(window, DXGI_MWA_NO_WINDOW_CHANGES | DXGI_MWA_NO_ALT_ENTER);
            if (desc.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) {
                swapchain->SetMaximumFrameLatency(1);
                waitable = swapchain->GetFrameLatencyWaitableObject();
            }
            if (first_present) {
                // The first present, here: its buffers are as they were made
                // (nothing was drawn). On Direct3D 12 the queue has run it
                // before the drawing thread gets the swap chain, so nothing
                // of it is in the GPU's hands when that thread resizes or
                // lets go.
                swapchain->Present(0, tearing ? DXGI_PRESENT_ALLOW_TEARING : 0u);
                ComPtr<ID3D12CommandQueue> queue;
                ComPtr<ID3D12Device> device;
                ComPtr<ID3D12Fence> fence;
                if (SUCCEEDED(device_or_queue.As(&queue)) && SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&device))) &&
                    SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))) &&
                    SUCCEEDED(queue->Signal(fence.Get(), 1))) {
                    if (HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr)) {
                        if (SUCCEEDED(fence->SetEventOnCompletion(1, event))) WaitForSingleObject(event, 2000);
                        CloseHandle(event);
                    }
                }
            }
        }
    }
    std::scoped_lock lock(making->mutex);
    making->busy = false;
    if (making->abandoned) {
        // Nobody is waiting for it any more.
        if (waitable) CloseHandle(waitable);
        return;
    }
    making->failed = !swapchain;
    making->error = std::move(error);
    making->swapchain = std::move(swapchain);
    making->waitable = waitable;
    making->flags = desc.Flags;
    making->tearing = tearing != FALSE;
}

MirrorSwapChain::State MirrorSwapChain::Ensure(const Request& request, const std::function<void()>& before_release,
                                               const std::function<void()>& after_release, bool& fresh,
                                               std::string& error) {
    fresh = false;
    for (int pass = 0; pass < 2; ++pass) {
        if (making_ && making_window_ != request.window) {
            // Being made for a window that is no longer the one: not waited
            // for (its window may be gone, and the task with it).
            std::scoped_lock lock(making_->mutex);
            making_->abandoned = true;
            if (making_->waitable) CloseHandle(making_->waitable);
            making_->waitable = nullptr;
            making_->swapchain.Reset();
        }
        if (making_ && making_window_ != request.window) making_.reset();
        if (making_) {
            std::unique_lock lock(making_->mutex);
            if (making_->busy) return State::kPending;
            if (making_->failed) {
                error = making_->error;
                lock.unlock();
                making_.reset();
                return State::kFailed;
            }
            swapchain_ = std::move(making_->swapchain);
            waitable_ = making_->waitable;
            making_->waitable = nullptr;
            flags_ = making_->flags;
            tearing_ = making_->tearing;
            lock.unlock();
            making_.reset();
            window_ = making_window_;
            width_ = making_width_;
            height_ = making_height_;
            waits_ = 0;
            fresh = true;
        }
        if (swapchain_ && window_ == request.window && width_ == request.width && height_ == request.height) {
            return State::kReady;
        }
        // None, or one of another window or picture size (new settings,
        // another monitor to fill): a new one, made on the window's thread.
        ComPtr<IDXGISwapChain3> replaced;
        if (swapchain_) {
            before_release();
            replaced = std::move(swapchain_);
            if (waitable_) CloseHandle(waitable_);
            waitable_ = nullptr;
            window_ = nullptr;
            width_ = 0;
            height_ = 0;
            if (request.release_on_caller) {
                replaced.Reset();
                after_release();
            }
        }
        fresh = false;
        making_ = std::make_shared<Making>();
        making_window_ = request.window;
        making_width_ = request.width;
        making_height_ = request.height;
        std::function<void()> task = [making = making_, factory = ComPtr<IDXGIFactory2>(request.factory),
                                      device = ComPtr<IUnknown>(request.device_or_queue), window = request.window,
                                      width = request.width, height = request.height,
                                      replaced = std::move(replaced),
                                      first_present = request.first_present_on_maker]() mutable {
            Make(making, std::move(factory), std::move(device), window, width, height, std::move(replaced),
                 first_present);
        };
        if (request.executor && *request.executor && (*request.executor)(task)) {
            return State::kPending;
        }
        // No thread of the window's to make it on: made here, and taken on
        // the next pass.
        task();
    }
    return making_ ? State::kPending : State::kReady;
}

bool MirrorSwapChain::Ready() {
    if (!waitable_) return true;
    if (WaitForSingleObject(waitable_, 0) == WAIT_OBJECT_0) {
        waits_ = 0;
        return true;
    }
    // A signal that never comes (a display that stopped taking frames) must
    // not end the picture for good: now and then a frame is presented anyway,
    // which does not wait either.
    if (++waits_ > 30) {
        waits_ = 0;
        return true;
    }
    return false;
}

bool MirrorSwapChain::Present(bool& busy, std::string& error) {
    busy = false;
    const HRESULT result =
        swapchain_->Present(0, DXGI_PRESENT_DO_NOT_WAIT | (tearing_ ? DXGI_PRESENT_ALLOW_TEARING : 0u));
    if (result == DXGI_ERROR_WAS_STILL_DRAWING) {
        busy = true;
        return true;
    }
    if (FAILED(result)) {
        error = Failure("Presenting to the window", result);
        return false;
    }
    return true;
}

void MirrorSwapChain::Drop() {
    if (waitable_) CloseHandle(waitable_);
    waitable_ = nullptr;
    swapchain_.Reset();
    window_ = nullptr;
    width_ = 0;
    height_ = 0;
    flags_ = 0;
    tearing_ = false;
    waits_ = 0;
}

void MirrorSwapChain::Reset() {
    if (making_) {
        std::scoped_lock lock(making_->mutex);
        // Still being made: the making thread lets go of it. Made and not
        // taken: let go of here.
        making_->abandoned = true;
        if (making_->waitable) CloseHandle(making_->waitable);
        making_->waitable = nullptr;
        making_->swapchain.Reset();
    }
    making_.reset();
    Drop();
}

// ---------------------------------------------------------------- Direct3D 11

D3D11Mirror::D3D11Mirror(ID3D11Device* device, ID3D11DeviceContext* context, bool context_states)
    : context_states_(context_states), device_(device), context_(context) {}

bool D3D11Mirror::Ensure(std::string& error) {
    if (ready_) return true;
    if (failed_) {
        error = "the Direct3D 11 mirror could not be set up";
        return false;
    }
    failed_ = true;
    HRESULT result = device_->CreateVertexShader(kMirrorVsDxbc, sizeof(kMirrorVsDxbc), nullptr, &vertex_shader_);
    if (SUCCEEDED(result)) {
        result = device_->CreatePixelShader(kMirrorPsDxbc, sizeof(kMirrorPsDxbc), nullptr, &pixel_shader_);
    }
    if (FAILED(result)) {
        error = Failure("Creating the mirror's shaders", result);
        return false;
    }
    D3D11_BUFFER_DESC buffer{};
    // Both pixel shaders' constants: the plain one reads the first part.
    buffer.ByteWidth = sizeof(MirrorFocusedShaderConstants);
    buffer.Usage = D3D11_USAGE_DEFAULT;
    buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    result = device_->CreateBuffer(&buffer, nullptr, &constants_);
    if (FAILED(result)) {
        error = Failure("Creating the mirror's constant buffer", result);
        return false;
    }
    // A context state of the pass's own, where the device has them: the
    // game's whole state comes back with one swap. Without, the draw's own
    // state is saved and restored.
    ComPtr<ID3D11Device1> device1;
    if (context_states_ && SUCCEEDED(device_.As(&device1)) && SUCCEEDED(context_.As(&context1_))) {
        const D3D_FEATURE_LEVEL level = device_->GetFeatureLevel();
        const UINT flags = (device_->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED)
                               ? D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED
                               : 0;
        if (FAILED(device1->CreateDeviceContextState(flags, &level, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device),
                                                     nullptr, &state_))) {
            state_.Reset();
        }
    }
    if (!state_) context1_.Reset();
    failed_ = false;
    ready_ = true;
    return true;
}

MirrorDrawResult D3D11Mirror::Prepare(const MirrorDraw& draw, std::string& error) {
    if (!Ensure(error)) return MirrorDrawResult::kFailed;
    if (!draw.window) return MirrorDrawResult::kShown;
    if (!factory_) {
        // Once: the device's factory does not change, and this is every
        // frame the window shows.
        ComPtr<IDXGIDevice> dxgi_device;
        ComPtr<IDXGIAdapter> adapter;
        HRESULT result = device_.As(&dxgi_device);
        if (SUCCEEDED(result)) result = dxgi_device->GetAdapter(&adapter);
        if (SUCCEEDED(result)) result = adapter->GetParent(IID_PPV_ARGS(&factory_));
        if (FAILED(result)) {
            factory_.Reset();
            error = Failure("Finding the device's DXGI factory", result);
            return MirrorDrawResult::kFailed;
        }
    }
    MirrorSwapChain::Request request;
    request.factory = factory_.Get();
    request.device_or_queue = device_.Get();
    request.window = static_cast<HWND>(draw.window);
    request.width = draw.width;
    request.height = draw.height;
    // Made on the window's thread, off the game's frame, only where the
    // game's device may be called from another thread: a device made
    // D3D11_CREATE_DEVICE_SINGLETHREADED serializes nothing, so its swap
    // chain is made here, on the drawing thread (a one-time hitch, and one
    // after each resize or monitor change).
    request.executor = (device_->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED) ? nullptr
                                                                                          : &draw.on_window_thread;
    // A swap chain that is replaced goes here, with the flush Direct3D 11
    // needs before its buffers are gone and the window can have another.
    request.release_on_caller = true;
    // A present uses the device's immediate context, which is the game's:
    // every present, the first included, is the drawing thread's. The
    // window's thread never presents on Direct3D 11: where the game's device
    // guards its context (multithread protection) a present there waits for
    // the device's lock, which the drawing thread (or the game) may hold
    // while it waits for the window's thread to end.
    request.first_present_on_maker = false;
    bool fresh = false;
    switch (swapchain_.Ensure(request, [this] { back_buffer_.Reset(); }, [this] { context_->Flush(); }, fresh, error)) {
    case MirrorSwapChain::State::kPending:
        back_buffer_.Reset();
        return MirrorDrawResult::kSkipped;
    case MirrorSwapChain::State::kFailed:
        back_buffer_.Reset();
        return MirrorDrawResult::kFailed;
    case MirrorSwapChain::State::kReady:
        break;
    }
    if (fresh) back_buffer_.Reset();
    return swapchain_.Ready() ? MirrorDrawResult::kShown : MirrorDrawResult::kSkipped;
}

void D3D11Mirror::BeginDraw() {
    if (state_) {
        context1_->SwapDeviceContextState(state_.Get(), &game_state_);
        context_->ClearState();
    } else {
        unpredicated_.emplace(context_.Get());
        Saved& s = saved_;
        s = {};
        context_->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, s.render_targets, &s.depth_stencil);
        context_->OMGetBlendState(&s.blend, s.blend_factor, &s.sample_mask);
        context_->OMGetDepthStencilState(&s.depth_state, &s.stencil_ref);
        context_->RSGetState(&s.rasterizer);
        s.viewport_count = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
        context_->RSGetViewports(&s.viewport_count, s.viewports);
        context_->VSGetShader(&s.vertex_shader, nullptr, nullptr);
        context_->PSGetShader(&s.pixel_shader, nullptr, nullptr);
        // The stages between the pass's two shaders: a geometry, hull or
        // domain shader the game left bound would take the pass's triangle.
        context_->GSGetShader(&s.geometry_shader, nullptr, nullptr);
        context_->HSGetShader(&s.hull_shader, nullptr, nullptr);
        context_->DSGetShader(&s.domain_shader, nullptr, nullptr);
        context_->GSSetShader(nullptr, nullptr, 0);
        context_->HSSetShader(nullptr, nullptr, 0);
        context_->DSSetShader(nullptr, nullptr, 0);
        context_->IAGetInputLayout(&s.input_layout);
        context_->IAGetPrimitiveTopology(&s.topology);
        context_->PSGetShaderResources(0, 1, &s.shader_resource);
        context_->PSGetConstantBuffers(0, 1, &s.constant_buffer);
        context_->OMSetBlendState(nullptr, nullptr, 0xffffffffu);
        context_->OMSetDepthStencilState(nullptr, 0);
        // The default rasterizer state has no scissor test, so the game's
        // scissor rectangles stay set and unused; its vertex and index
        // buffers stay bound and unread (no input layout, Draw).
        context_->RSSetState(nullptr);
    }
}

void D3D11Mirror::EndDraw() {
    ID3D11ShaderResourceView* no_source = nullptr;
    context_->PSSetShaderResources(0, 1, &no_source);
    context_->OMSetRenderTargets(0, nullptr, nullptr);
    if (state_) {
        context1_->SwapDeviceContextState(game_state_.Get(), nullptr);
        game_state_.Reset();
        return;
    }
    Saved& s = saved_;
    context_->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, s.render_targets, s.depth_stencil);
    context_->OMSetBlendState(s.blend, s.blend_factor, s.sample_mask);
    context_->OMSetDepthStencilState(s.depth_state, s.stencil_ref);
    context_->RSSetState(s.rasterizer);
    context_->RSSetViewports(s.viewport_count, s.viewports);
    context_->VSSetShader(s.vertex_shader, nullptr, 0);
    context_->PSSetShader(s.pixel_shader, nullptr, 0);
    context_->GSSetShader(s.geometry_shader, nullptr, 0);
    context_->HSSetShader(s.hull_shader, nullptr, 0);
    context_->DSSetShader(s.domain_shader, nullptr, 0);
    context_->IASetInputLayout(s.input_layout);
    context_->IASetPrimitiveTopology(s.topology);
    context_->PSSetShaderResources(0, 1, &s.shader_resource);
    context_->PSSetConstantBuffers(0, 1, &s.constant_buffer);
    for (ID3D11RenderTargetView* view : s.render_targets) {
        if (view) view->Release();
    }
    const auto release = [](IUnknown* object) {
        if (object) object->Release();
    };
    release(s.depth_stencil);
    release(s.blend);
    release(s.depth_state);
    release(s.rasterizer);
    release(s.vertex_shader);
    release(s.pixel_shader);
    release(s.geometry_shader);
    release(s.hull_shader);
    release(s.domain_shader);
    release(s.input_layout);
    release(s.shader_resource);
    release(s.constant_buffer);
    s = {};
    unpredicated_.reset();
}

ID3D11ShaderResourceView* D3D11Mirror::Readable(ID3D11Texture2D* source, const D3D11_TEXTURE2D_DESC& source_desc,
                                                 const SubmittedView& view, DXGI_FORMAT source_view_format, Copy& copy,
                                                 std::int32_t& offset_x, std::int32_t& offset_y, std::string& error) {
    offset_x = view.rect.offset.x;
    offset_y = view.rect.offset.y;
    if (source_desc.BindFlags & D3D11_BIND_SHADER_RESOURCE) {
        const std::uint64_t key = (static_cast<std::uint64_t>(view.array_slice) << 32) |
                                  static_cast<std::uint32_t>(source_view_format);
        for (const Source& existing : sources_) {
            if (existing.texture == source && existing.key == key) return existing.view.Get();
        }
        D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
        desc.Format = source_view_format;
        desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        desc.Texture2DArray.MipLevels = 1;
        desc.Texture2DArray.FirstArraySlice = view.array_slice;
        desc.Texture2DArray.ArraySize = 1;
        ComPtr<ID3D11ShaderResourceView> created;
        const HRESULT result = device_->CreateShaderResourceView(source, &desc, &created);
        if (FAILED(result)) {
            error = Failure("Creating the source view", result);
            return nullptr;
        }
        ID3D11ShaderResourceView* source_view = created.Get();
        sources_.push_back(Source{source, key, std::move(created)});
        return source_view;
    }
    // Not bindable for reading: the rectangle is copied to a texture that
    // is, kept while the rectangle fits in it.
    const auto rect_width = static_cast<std::uint32_t>(view.rect.extent.width);
    const auto rect_height = static_cast<std::uint32_t>(view.rect.extent.height);
    if (!copy.texture || copy.width < rect_width || copy.height < rect_height || copy.format != source_view_format) {
        copy = {};
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = rect_width;
        desc.Height = rect_height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = source_view_format;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        HRESULT result = device_->CreateTexture2D(&desc, nullptr, &copy.texture);
        if (SUCCEEDED(result)) {
            D3D11_SHADER_RESOURCE_VIEW_DESC view_desc{};
            view_desc.Format = source_view_format;
            view_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
            view_desc.Texture2DArray.MipLevels = 1;
            view_desc.Texture2DArray.ArraySize = 1;
            result = device_->CreateShaderResourceView(copy.texture.Get(), &view_desc, &copy.view);
        }
        if (FAILED(result)) {
            copy = {};
            error = Failure("Creating the source copy", result);
            return nullptr;
        }
        copy.width = rect_width;
        copy.height = rect_height;
        copy.format = source_view_format;
    }
    const D3D11_BOX box{static_cast<UINT>(view.rect.offset.x),
                        static_cast<UINT>(view.rect.offset.y),
                        0,
                        static_cast<UINT>(view.rect.offset.x + view.rect.extent.width),
                        static_cast<UINT>(view.rect.offset.y + view.rect.extent.height),
                        1};
    context_->CopySubresourceRegion(copy.texture.Get(), 0, 0, 0, 0, source,
                                    D3D11CalcSubresource(0, view.array_slice, source_desc.MipLevels), &box);
    offset_x = 0;
    offset_y = 0;
    return copy.view.Get();
}

bool D3D11Mirror::Draw(ID3D11Texture2D* source, const SubmittedView& view, const SubmittedFormat& source_format,
                       ID3D11Texture2D* target, const MirrorDraw& draw, std::string& error, ID3D11Texture2D* focus) {
    if (!Ensure(error)) return false;
    struct DrawScope {
        D3D11Mirror& mirror;
        ~DrawScope() { mirror.EndDraw(); }
    };
    BeginDraw();
    const DrawScope draw_scope{*this};
    D3D11_TEXTURE2D_DESC source_desc{};
    source->GetDesc(&source_desc);
    if (!CheckDraw(view, source_format, source_desc.Format, source_desc.Width, source_desc.Height,
                   source_desc.ArraySize, source_desc.SampleDesc.Count, draw, error)) {
        return false;
    }
    const auto source_view_format = static_cast<DXGI_FORMAT>(source_format.view_format);
    std::int32_t offset_x = 0;
    std::int32_t offset_y = 0;
    ID3D11ShaderResourceView* source_view =
        Readable(source, source_desc, view, source_view_format, copy_, offset_x, offset_y, error);
    if (!source_view) return false;

    // The eye's focus view, drawn over it.
    const bool focused = focus && draw.focus.present;
    ID3D11ShaderResourceView* focus_view = nullptr;
    std::int32_t focus_offset_x = 0;
    std::int32_t focus_offset_y = 0;
    if (focused) {
        const SubmittedView& focus_image = draw.focus.view;
        const SubmittedFormat focus_format = ClassifyFormat(SubmittedImageApi::kD3D11, focus_image.format);
        D3D11_TEXTURE2D_DESC focus_desc{};
        focus->GetDesc(&focus_desc);
        if (!CheckDraw(focus_image, focus_format, focus_desc.Format, focus_desc.Width, focus_desc.Height,
                       focus_desc.ArraySize, focus_desc.SampleDesc.Count, draw, error)) {
            error = "the focus view: " + error;
            return false;
        }
        if (LinearTexels(focus_format) != LinearTexels(source_format)) {
            error = "the focus view's texels are not encoded as the eye's";
            return false;
        }
        if (!focus_pixel_shader_) {
            const HRESULT result = device_->CreatePixelShader(kMirrorFocusPsDxbc, sizeof(kMirrorFocusPsDxbc), nullptr,
                                                              &focus_pixel_shader_);
            if (FAILED(result)) {
                error = Failure("Creating the mirror's focus shader", result);
                return false;
            }
        }
        focus_view = Readable(focus, focus_desc, focus_image, static_cast<DXGI_FORMAT>(focus_format.view_format),
                              focus_copy_, focus_offset_x, focus_offset_y, error);
        if (!focus_view) return false;
    }

    ComPtr<ID3D11RenderTargetView> target_view;
    if (target) {
        D3D11_TEXTURE2D_DESC target_desc{};
        target->GetDesc(&target_desc);
        const SubmittedFormat target_format = ClassifyFormat(SubmittedImageApi::kD3D11, target_desc.Format);
        if (!(target_desc.BindFlags & D3D11_BIND_RENDER_TARGET) || target_desc.SampleDesc.Count != 1 ||
            target_desc.Width < draw.width || target_desc.Height < draw.height ||
            target_format.kind != SubmittedPixelKind::kUnorm8 || target_format.srgb) {
            error = "the target image cannot be drawn into";
            return false;
        }
        D3D11_RENDER_TARGET_VIEW_DESC desc{};
        desc.Format = static_cast<DXGI_FORMAT>(target_format.view_format);
        if (target_desc.ArraySize > 1) {
            desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
            desc.Texture2DArray.ArraySize = 1;
        } else {
            desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        }
        const HRESULT result = device_->CreateRenderTargetView(target, &desc, &target_view);
        if (FAILED(result)) {
            error = Failure("Creating the target view", result);
            return false;
        }
    } else {
        if (!swapchain_) {
            error = "the window has no swap chain";
            return false;
        }
        if (!back_buffer_) {
            // Buffer 0 is the one to draw into after every present.
            ComPtr<ID3D11Texture2D> buffer;
            HRESULT result = swapchain_.Get()->GetBuffer(0, IID_PPV_ARGS(&buffer));
            if (SUCCEEDED(result)) result = device_->CreateRenderTargetView(buffer.Get(), nullptr, &back_buffer_);
            if (FAILED(result)) {
                back_buffer_.Reset();
                error = Failure("Creating the window's render target", result);
                return false;
            }
        }
        target_view = back_buffer_;
    }

    MirrorFocusedShaderConstants packed{};
    packed.eye = PackMirrorConstants(draw.homography, offset_x, offset_y, view.rect.extent.width,
                                     view.rect.extent.height, draw.width, draw.height, LinearTexels(source_format));
    if (focused) {
        packed.focus = PackMirrorFocusConstants(draw.homography, draw.focus.place, focus_offset_x, focus_offset_y,
                                                draw.focus.view.rect.extent.width, draw.focus.view.rect.extent.height,
                                                draw.width, draw.height);
    }
    context_->UpdateSubresource(constants_.Get(), 0, nullptr, &packed, 0, 0);
    ID3D11RenderTargetView* render_target = target_view.Get();
    context_->OMSetRenderTargets(1, &render_target, nullptr);
    D3D11_VIEWPORT viewport{};
    viewport.Width = static_cast<float>(draw.width);
    viewport.Height = static_cast<float>(draw.height);
    viewport.MaxDepth = 1.0f;
    context_->RSSetViewports(1, &viewport);
    context_->IASetInputLayout(nullptr);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->VSSetShader(vertex_shader_.Get(), nullptr, 0);
    context_->PSSetShader(focused ? focus_pixel_shader_.Get() : pixel_shader_.Get(), nullptr, 0);
    // The state guard around this draw puts the game's first resource slot
    // back where it saves and restores by hand; the second, which only a
    // focus view uses, is put back here.
    ComPtr<ID3D11ShaderResourceView> second_slot;
    if (focused) context_->PSGetShaderResources(1, 1, &second_slot);
    ID3D11ShaderResourceView* const read[2] = {source_view, focus_view};
    context_->PSSetShaderResources(0, focused ? 2 : 1, read);
    ID3D11Buffer* buffer = constants_.Get();
    context_->PSSetConstantBuffers(0, 1, &buffer);
    context_->Draw(3, 0);
    ID3D11ShaderResourceView* const no_source = nullptr;
    context_->PSSetShaderResources(0, 1, &no_source);
    if (focused) {
        ID3D11ShaderResourceView* const before = second_slot.Get();
        context_->PSSetShaderResources(1, 1, &before);
    }
    context_->OMSetRenderTargets(0, nullptr, nullptr);
    return true;
}

MirrorDrawResult D3D11Mirror::Present(std::string& error) {
    if (!swapchain_) {
        error = "the window has no swap chain";
        return MirrorDrawResult::kFailed;
    }
    bool busy = false;
    if (!swapchain_.Present(busy, error)) return MirrorDrawResult::kFailed;
    return busy ? MirrorDrawResult::kSkipped : MirrorDrawResult::kShown;
}

void D3D11Mirror::Forget(ID3D11Texture2D* texture) {
    std::erase_if(sources_, [&](const Source& source) { return source.texture == texture; });
}

void D3D11Mirror::Close() {
    back_buffer_.Reset();
    const bool held = static_cast<bool>(swapchain_);
    // One still being made is let go of by the window's thread.
    swapchain_.Reset();
    // The buffers go with the flush, before the window does.
    if (held) context_->Flush();
}

void D3D11Mirror::Reset() {
    Close();
    sources_.clear();
    copy_ = {};
    focus_copy_ = {};
}

// ---------------------------------------------------------------- Direct3D 12

D3D12Mirror::D3D12Mirror(ID3D12Device* device, ID3D12CommandQueue* queue) : device_(device), queue_(queue) {}

// On the window's thread (or the caller's, when the window's cannot take it):
// Direct3D 12's creation calls are free-threaded.
void D3D12Mirror::Build(const std::shared_ptr<Setup>& setup, ComPtr<ID3D12Device> device) {
    Setup made;
    std::string error;
    HRESULT result = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&made.fence));
    if (SUCCEEDED(result)) {
        result = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&made.allocator));
    }
    if (SUCCEEDED(result)) {
        result = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, made.allocator.Get(), nullptr,
                                           IID_PPV_ARGS(&made.commands));
    }
    if (SUCCEEDED(result)) result = made.commands->Close();
    if (FAILED(result)) error = Failure("Creating the mirror's fence, allocator and command list", result);
    if (error.empty()) {
        D3D12_DESCRIPTOR_RANGE range{};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = 1;
        D3D12_ROOT_PARAMETER parameters[2]{};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[0].Constants.Num32BitValues = sizeof(MirrorShaderConstants) / 4;
        parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[1].DescriptorTable.NumDescriptorRanges = 1;
        parameters[1].DescriptorTable.pDescriptorRanges = &range;
        parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC root{};
        root.NumParameters = 2;
        root.pParameters = parameters;
        ComPtr<ID3DBlob> serialized;
        ComPtr<ID3DBlob> diagnostics;
        result = D3D12SerializeRootSignature(&root, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &diagnostics);
        if (SUCCEEDED(result)) {
            result = device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                                 IID_PPV_ARGS(&made.root));
        }
        if (FAILED(result)) error = Failure("Creating the mirror's root signature", result);
    }
    if (error.empty()) {
        D3D12_DESCRIPTOR_HEAP_DESC heap{};
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap.NumDescriptors = kSourceSlots;
        heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        result = device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&made.source_heap));
        if (SUCCEEDED(result)) {
            heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            heap.NumDescriptors = 1;
            heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
            result = device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&made.target_heap));
        }
        if (FAILED(result)) error = Failure("Creating the mirror's descriptor heaps", result);
    }
    if (error.empty()) {
        // The window's pipeline, the one that takes time to make.
        made.window_pipeline = MakePipeline(device.Get(), made.root.Get(), MirrorSwapChain::kFormat, error);
    }
    std::scoped_lock lock(setup->mutex);
    setup->busy = false;
    setup->failed = !error.empty();
    setup->error = std::move(error);
    if (!setup->failed) {
        setup->fence = std::move(made.fence);
        setup->root = std::move(made.root);
        setup->source_heap = std::move(made.source_heap);
        setup->target_heap = std::move(made.target_heap);
        setup->allocator = std::move(made.allocator);
        setup->commands = std::move(made.commands);
        setup->window_pipeline = std::move(made.window_pipeline);
    }
}

D3D12Mirror::Readiness D3D12Mirror::Ensure(const std::function<bool(std::function<void()>)>* executor,
                                           std::string& error) {
    if (ready_) return Readiness::kReady;
    if (failed_) {
        error = "the Direct3D 12 mirror could not be set up";
        return Readiness::kFailed;
    }
    if (!setup_) {
        setup_ = std::make_shared<Setup>();
        std::function<void()> task = [setup = setup_, device = device_] { Build(setup, device); };
        if (executor && *executor && (*executor)(task)) return Readiness::kPending;
        task();
    }
    std::unique_lock lock(setup_->mutex);
    if (setup_->busy) return Readiness::kPending;
    if (setup_->failed) {
        error = setup_->error;
        lock.unlock();
        setup_.reset();
        failed_ = true;
        return Readiness::kFailed;
    }
    fence_ = std::move(setup_->fence);
    root_ = std::move(setup_->root);
    source_heap_ = std::move(setup_->source_heap);
    target_heap_ = std::move(setup_->target_heap);
    allocator_ = std::move(setup_->allocator);
    commands_ = std::move(setup_->commands);
    pipelines_.emplace_back(static_cast<int>(MirrorSwapChain::kFormat), std::move(setup_->window_pipeline));
    lock.unlock();
    setup_.reset();
    source_step_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    target_step_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    sources_.assign(kSourceSlots, {});
    ready_ = true;
    return Readiness::kReady;
}

bool D3D12Mirror::Idle() const {
    return !fence_ || fence_->GetCompletedValue() >= serial_;
}

bool D3D12Mirror::WaitIdle(DWORD milliseconds) {
    if (Idle()) return true;
    if (FAILED(device_->GetDeviceRemovedReason())) return true;
    if (HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr)) {
        if (SUCCEEDED(fence_->SetEventOnCompletion(serial_, event))) WaitForSingleObject(event, milliseconds);
        CloseHandle(event);
    }
    return Idle() || FAILED(device_->GetDeviceRemovedReason());
}

ComPtr<ID3D12PipelineState> D3D12Mirror::MakePipeline(ID3D12Device* device, ID3D12RootSignature* root,
                                                      DXGI_FORMAT target_format, std::string& error) {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = root;
    desc.VS = {kMirrorVsDxbc, sizeof(kMirrorVsDxbc)};
    desc.PS = {kMirrorPsDxbc, sizeof(kMirrorPsDxbc)};
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.SampleMask = 0xffffffffu;
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = target_format;
    desc.SampleDesc.Count = 1;
    ComPtr<ID3D12PipelineState> pipeline;
    const HRESULT result = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline));
    if (FAILED(result)) {
        error = Failure("Creating the mirror's pipeline", result);
        return nullptr;
    }
    return pipeline;
}

ID3D12PipelineState* D3D12Mirror::Pipeline(DXGI_FORMAT target_format, std::string& error) {
    for (const auto& [format, pipeline] : pipelines_) {
        if (format == static_cast<int>(target_format)) return pipeline.Get();
    }
    // Another format than the window's: a tracked image drawn into (tests).
    ComPtr<ID3D12PipelineState> pipeline = MakePipeline(device_.Get(), root_.Get(), target_format, error);
    if (!pipeline) return nullptr;
    pipelines_.emplace_back(static_cast<int>(target_format), std::move(pipeline));
    return pipelines_.back().second.Get();
}

void D3D12Mirror::Retire(ComPtr<ID3D12Resource> image) {
    if (!ready_ || !image) return;
    for (SourceView& slot : sources_) {
        if (slot.image == image.Get()) slot = {};
    }
    if (!Idle()) retired_.emplace_back(serial_, std::move(image));
}

MirrorDrawResult D3D12Mirror::Draw(ID3D12Resource* source, const SubmittedView& view,
                                   const SubmittedFormat& source_format, ID3D12Resource* target,
                                   const MirrorDraw& draw, std::string& error, D3D12_RESOURCE_STATES source_rest) {
    // Its own objects are made on the window's thread where there is one.
    switch (Ensure(target ? nullptr : &draw.on_window_thread, error)) {
    case Readiness::kPending:
        return MirrorDrawResult::kSkipped;
    case Readiness::kFailed:
        return MirrorDrawResult::kFailed;
    case Readiness::kReady:
        break;
    }
    HRESULT result = device_->GetDeviceRemovedReason();
    if (FAILED(result)) {
        error = Failure("The Direct3D 12 device", result);
        return MirrorDrawResult::kFailed;
    }
    // One frame in the GPU's hands at a time: while it has not finished the
    // last, this one is not shown (and the one allocator is still in use).
    if (!Idle()) return MirrorDrawResult::kSkipped;
    retired_.clear();

    const D3D12_RESOURCE_DESC source_desc = source->GetDesc();
    if (source_desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) {
        error = "the image is not a 2D texture";
        return MirrorDrawResult::kFailed;
    }
    if (!CheckDraw(view, source_format, source_desc.Format, source_desc.Width, source_desc.Height,
                   source_desc.DepthOrArraySize, source_desc.SampleDesc.Count, draw, error)) {
        return MirrorDrawResult::kFailed;
    }
    if (source_desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) {
        error = "the image cannot be read by a shader";
        return MirrorDrawResult::kFailed;
    }

    ID3D12Resource* drawn = target;
    DXGI_FORMAT target_format = MirrorSwapChain::kFormat;
    D3D12_RESOURCE_STATES target_rest = D3D12_RESOURCE_STATE_RENDER_TARGET;
    if (target) {
        const D3D12_RESOURCE_DESC target_desc = target->GetDesc();
        const SubmittedFormat classified = ClassifyFormat(SubmittedImageApi::kD3D12, target_desc.Format);
        if (target_desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
            !(target_desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) || target_desc.SampleDesc.Count != 1 ||
            target_desc.Width < draw.width || target_desc.Height < draw.height ||
            classified.kind != SubmittedPixelKind::kUnorm8 || classified.srgb) {
            error = "the target image cannot be drawn into";
            return MirrorDrawResult::kFailed;
        }
        target_format = static_cast<DXGI_FORMAT>(classified.view_format);
    } else {
        if (!draw.window) {
            error = "no window to draw into";
            return MirrorDrawResult::kFailed;
        }
        MirrorSwapChain::Request request;
        request.device_or_queue = queue_.Get();
        request.window = static_cast<HWND>(draw.window);
        request.width = draw.width;
        request.height = draw.height;
        request.executor = &draw.on_window_thread;
        request.first_present_on_maker = true;
        const auto drop_buffers = [this] {
            for (ComPtr<ID3D12Resource>& buffer : buffers_) buffer.Reset();
        };
        bool fresh = false;
        // The GPU is done with the last frame and its present (Idle above),
        // so a swap chain that is replaced can go on the window's thread.
        switch (swapchain_.Ensure(request, drop_buffers, [] {}, fresh, error)) {
        case MirrorSwapChain::State::kPending:
            return MirrorDrawResult::kSkipped;
        case MirrorSwapChain::State::kFailed:
            drop_buffers();
            return MirrorDrawResult::kFailed;
        case MirrorSwapChain::State::kReady:
            break;
        }
        if (fresh || !buffers_[0]) {
            for (UINT i = 0; i < MirrorSwapChain::kBuffers; ++i) {
                buffers_[i].Reset();
                result = swapchain_.Get()->GetBuffer(i, IID_PPV_ARGS(&buffers_[i]));
                if (FAILED(result)) {
                    drop_buffers();
                    error = Failure("Getting the window's buffers", result);
                    return MirrorDrawResult::kFailed;
                }
            }
        }
        if (!swapchain_.Ready()) return MirrorDrawResult::kSkipped;
        drawn = buffers_[swapchain_.Get()->GetCurrentBackBufferIndex() % MirrorSwapChain::kBuffers].Get();
        target_rest = D3D12_RESOURCE_STATE_PRESENT;
    }

    ID3D12PipelineState* pipeline = Pipeline(target_format, error);
    if (!pipeline) return MirrorDrawResult::kFailed;

    // The GPU is done with every earlier draw, so any slot may be rewritten.
    const auto source_view_format = static_cast<DXGI_FORMAT>(source_format.view_format);
    int slot = -1;
    for (int i = 0; i < static_cast<int>(sources_.size()); ++i) {
        const SourceView& existing = sources_[i];
        if (existing.image == source && existing.slice == view.array_slice && existing.format == source_view_format) {
            slot = i;
        }
    }
    if (slot < 0) {
        slot = static_cast<int>(next_source_);
        next_source_ = (next_source_ + 1) % kSourceSlots;
        D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
        desc.Format = source_view_format;
        desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        desc.Texture2DArray.MipLevels = 1;
        desc.Texture2DArray.FirstArraySlice = view.array_slice;
        desc.Texture2DArray.ArraySize = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE handle = source_heap_->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(slot) * source_step_;
        device_->CreateShaderResourceView(source, &desc, handle);
        sources_[slot] = SourceView{source, view.array_slice, source_view_format};
    }

    result = allocator_->Reset();
    if (SUCCEEDED(result)) {
        if (!commands_) {
            // The list a failed Close left unusable.
            result = device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator_.Get(), nullptr,
                                                IID_PPV_ARGS(&commands_));
        } else {
            result = commands_->Reset(allocator_.Get(), nullptr);
        }
    }
    if (FAILED(result)) {
        error = Failure("Starting the mirror's command list", result);
        return MirrorDrawResult::kFailed;
    }
    D3D12_RENDER_TARGET_VIEW_DESC target_view{};
    target_view.Format = target_format;
    if (drawn->GetDesc().DepthOrArraySize > 1) {
        target_view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
        target_view.Texture2DArray.ArraySize = 1;
    } else {
        target_view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    }
    const D3D12_CPU_DESCRIPTOR_HANDLE target_handle = target_heap_->GetCPUDescriptorHandleForHeapStart();
    device_->CreateRenderTargetView(drawn, &target_view, target_handle);

    ID3D12DescriptorHeap* heaps[] = {source_heap_.Get()};
    commands_->SetDescriptorHeaps(1, heaps);
    commands_->SetGraphicsRootSignature(root_.Get());
    commands_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const UINT source_subresource = view.array_slice * std::max<UINT>(source_desc.MipLevels, 1);
    D3D12_RESOURCE_BARRIER barriers[2] = {
        Transition(source, source_subresource, source_rest, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE),
        Transition(drawn, 0, target_rest, D3D12_RESOURCE_STATE_RENDER_TARGET)};
    // A simultaneous-access source resting in COMMON is promoted to the
    // read state by the draw itself and decays after it: no barrier.
    const bool promoted = source_rest == D3D12_RESOURCE_STATE_COMMON &&
                          (source_desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS) != 0;
    const bool target_barrier = target_rest != D3D12_RESOURCE_STATE_RENDER_TARGET;
    if (promoted) barriers[0] = barriers[1];
    const UINT barrier_count = (promoted ? 0u : 1u) + (target_barrier ? 1u : 0u);
    if (barrier_count) commands_->ResourceBarrier(barrier_count, barriers);
    commands_->OMSetRenderTargets(1, &target_handle, FALSE, nullptr);
    const D3D12_VIEWPORT viewport{0.0f, 0.0f, static_cast<float>(draw.width), static_cast<float>(draw.height),
                                  0.0f, 1.0f};
    const D3D12_RECT scissor{0, 0, static_cast<LONG>(draw.width), static_cast<LONG>(draw.height)};
    commands_->RSSetViewports(1, &viewport);
    commands_->RSSetScissorRects(1, &scissor);
    commands_->SetPipelineState(pipeline);
    const MirrorShaderConstants packed =
        PackMirrorConstants(draw.homography, view.rect.offset.x, view.rect.offset.y, view.rect.extent.width,
                            view.rect.extent.height, draw.width, draw.height, LinearTexels(source_format));
    commands_->SetGraphicsRoot32BitConstants(0, sizeof(packed) / 4, &packed, 0);
    D3D12_GPU_DESCRIPTOR_HANDLE source_handle = source_heap_->GetGPUDescriptorHandleForHeapStart();
    source_handle.ptr += static_cast<UINT64>(slot) * source_step_;
    commands_->SetGraphicsRootDescriptorTable(1, source_handle);
    commands_->DrawInstanced(3, 1, 0, 0);
    for (UINT i = 0; i < barrier_count; ++i) {
        std::swap(barriers[i].Transition.StateBefore, barriers[i].Transition.StateAfter);
    }
    if (barrier_count) commands_->ResourceBarrier(barrier_count, barriers);
    result = commands_->Close();
    if (FAILED(result)) {
        // The list is unusable as it is: made again at the next draw.
        commands_.Reset();
        error = Failure("Closing the mirror's command list", result);
        return MirrorDrawResult::kFailed;
    }
    ID3D12CommandList* lists[] = {commands_.Get()};
    queue_->ExecuteCommandLists(1, lists);
    // The window's frame is queued behind the draw, and the fence behind
    // both: the buffers are the GPU's until the present has run too.
    bool busy = false;
    std::string present_error;
    const bool presented = target || swapchain_.Present(busy, present_error);
    const std::uint64_t serial = serial_ + 1;
    result = queue_->Signal(fence_.Get(), serial);
    if (FAILED(result)) {
        // Executed with no way to learn when: nothing of it is used again.
        failed_ = true;
        ready_ = false;
        serial_ = UINT64_MAX;
        error = Failure("Signalling the mirror's fence", result);
        return MirrorDrawResult::kFailed;
    }
    serial_ = serial;
    if (!presented) {
        error = present_error;
        return MirrorDrawResult::kFailed;
    }
    return busy ? MirrorDrawResult::kSkipped : MirrorDrawResult::kShown;
}

void D3D12Mirror::Close() {
    if (!swapchain_) {
        // One still being made is let go of by the window's thread.
        swapchain_.Reset();
        return;
    }
    if (!WaitIdle(2000)) {
        // Never freed under the GPU: left to the device.
        for (ComPtr<ID3D12Resource>& buffer : buffers_) buffer.Detach();
        ComPtr<IDXGISwapChain3> kept = swapchain_.Get();
        kept.Detach();
    }
    for (ComPtr<ID3D12Resource>& buffer : buffers_) buffer.Reset();
    swapchain_.Reset();
}

void D3D12Mirror::Reset() {
    Close();
    // Objects still being made are let go of by the thread making them.
    setup_.reset();
    const bool done = serial_ != UINT64_MAX && WaitIdle(5000);
    if (!done) {
        allocator_.Detach();
        commands_.Detach();
        for (auto& [serial, image] : retired_) image.Detach();
        for (auto& [format, pipeline] : pipelines_) pipeline.Detach();
        source_heap_.Detach();
        target_heap_.Detach();
        root_.Detach();
    }
    allocator_.Reset();
    commands_.Reset();
    retired_.clear();
    pipelines_.clear();
    source_heap_.Reset();
    target_heap_.Reset();
    root_.Reset();
    fence_.Reset();
    sources_.clear();
    serial_ = 0;
    next_source_ = 0;
    ready_ = false;
    failed_ = false;
}

} // namespace depthxr
