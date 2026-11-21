#pragma once

// The stabilized mirror window's draws for the Direct3D backends of
// submitted_image_gpu.cpp (shaders/mirror.hlsl): one view of the frame the
// game submits, drawn through a homography into a desktop window's swap
// chain on the game's own device and presented without waiting. Internal to
// the imaging library.

#include <windows.h>
#include <d3d11_1.h>
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_5.h>
#include <wrl/client.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "depthxr/d3d11_predication.h"
#include "depthxr/submitted_image.h"

namespace depthxr {

// A window's flip-model swap chain, made for a Direct3D 11 device or a
// Direct3D 12 queue: two 8-bit BGRA buffers that hold display-encoded
// values, never full screen, presented with no wait for the display. It is
// made on the window's own thread, never on the thread that draws: making
// one takes tens of milliseconds. It has the picture's size, which only the
// settings change, and Windows stretches it to the window (DXGI_SCALING_STRETCH):
// resizing the window makes no new one.
class MirrorSwapChain {
  public:
    enum class State {
        kReady,   // there, of the window's size
        kPending, // being made on the window's thread: nothing to draw into yet
        kFailed,
    };
    struct Request {
        // Null: made on the thread that makes the swap chain.
        IDXGIFactory2* factory{nullptr};
        // What the swap chain presents with: the Direct3D 11 device or the
        // Direct3D 12 queue.
        IUnknown* device_or_queue{nullptr};
        HWND window{nullptr};
        std::uint32_t width{0};
        std::uint32_t height{0};
        // MirrorDraw::on_window_thread; null or refusing, the caller makes it.
        const std::function<bool(std::function<void()>)>* executor{nullptr};
        // Direct3D 11: a swap chain that is replaced is let go of by the
        // caller, whose context must be flushed (`after_release`) before the
        // window can have another. Otherwise it goes on the making thread.
        bool release_on_caller{false};
        // The making thread also presents the new swap chain once, empty
        // (and on Direct3D 12 waits for the queue to have run that). A swap
        // chain's first present takes Windows ten milliseconds and more,
        // which then is not the drawing thread's. Direct3D 12 only (the
        // queue is free-threaded); never Direct3D 11, whose presents use the
        // game's immediate context and, where the device guards it, its
        // lock.
        bool first_present_on_maker{false};
    };
    // The swap chain for the window at the picture's size. One that is
    // missing, of another window or of another size is made anew on the
    // window's thread: the call returns kPending at once and goes on doing
    // so until it is there (`fresh` on the first kReady after). Before one
    // in use is let go of, `before_release` drops the caller's views of its
    // buffers. Never waits.
    State Ensure(const Request& request, const std::function<void()>& before_release,
                 const std::function<void()>& after_release, bool& fresh, std::string& error);
    // Whether a frame can be presented now without waiting for the display.
    bool Ready();
    // DXGI_ERROR_WAS_STILL_DRAWING is `busy`, not a failure.
    bool Present(bool& busy, std::string& error);
    // Lets go of the swap chain; one still being made is let go of by the
    // thread making it, when it is done.
    void Reset();
    IDXGISwapChain3* Get() const { return swapchain_.Get(); }
    HWND Window() const { return window_; }
    explicit operator bool() const { return swapchain_ != nullptr; }
    static constexpr DXGI_FORMAT kFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
    static constexpr UINT kBuffers = 2;

  private:
    // What the making thread hands back.
    struct Making {
        std::mutex mutex;
        bool busy{true};
        bool abandoned{false};
        bool failed{false};
        std::string error;
        Microsoft::WRL::ComPtr<IDXGISwapChain3> swapchain;
        HANDLE waitable{nullptr};
        UINT flags{0};
        bool tearing{false};
    };
    static void Make(const std::shared_ptr<Making>& making, Microsoft::WRL::ComPtr<IDXGIFactory2> factory,
                     Microsoft::WRL::ComPtr<IUnknown> device_or_queue, HWND window, std::uint32_t width,
                     std::uint32_t height, Microsoft::WRL::ComPtr<IDXGISwapChain3> replaced, bool first_present);
    void Drop();

    std::shared_ptr<Making> making_;
    Microsoft::WRL::ComPtr<IDXGISwapChain3> swapchain_;
    HWND window_{nullptr};
    HANDLE waitable_{nullptr};
    std::uint32_t width_{0};
    std::uint32_t height_{0};
    // What `making_` is being made for.
    HWND making_window_{nullptr};
    std::uint32_t making_width_{0};
    std::uint32_t making_height_{0};
    UINT flags_{0};
    bool tearing_{false};
    std::uint32_t waits_{0};
};

// Direct3D 11: a draw on the game's immediate context between the its state guard, which puts the
// game's state back.
class D3D11Mirror {
  public:
    D3D11Mirror(ID3D11Device* device, ID3D11DeviceContext* context, bool context_states);
    // Before the state guard: the window's swap chain, asked for or taken.
    // kSkipped while it is being made and when a frame cannot be presented
    // now.
    MirrorDrawResult Prepare(const MirrorDraw& draw, std::string& error);
    // `target`: a texture to draw into instead of the window (tests, and a
    // screenshot of a quad views eye with its focus view). `focus`: the
    // image of draw.focus's view, drawn over the eye where it is present.
    bool Draw(ID3D11Texture2D* source, const SubmittedView& view, const SubmittedFormat& source_format,
              ID3D11Texture2D* target, const MirrorDraw& draw, std::string& error, ID3D11Texture2D* focus = nullptr);
    // After the state guard.
    MirrorDrawResult Present(std::string& error);
    void Forget(ID3D11Texture2D* texture);
    void Close();
    void Reset();

  private:
    struct Saved {
        ID3D11RenderTargetView* render_targets[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
        ID3D11DepthStencilView* depth_stencil{nullptr};
        ID3D11VertexShader* vertex_shader{nullptr};
        ID3D11PixelShader* pixel_shader{nullptr};
        ID3D11GeometryShader* geometry_shader{nullptr};
        ID3D11HullShader* hull_shader{nullptr};
        ID3D11DomainShader* domain_shader{nullptr};
        ID3D11InputLayout* input_layout{nullptr};
        ID3D11ShaderResourceView* shader_resource{nullptr};
        ID3D11Buffer* constant_buffer{nullptr};
        ID3D11BlendState* blend{nullptr};
        float blend_factor[4]{};
        UINT sample_mask{0};
        ID3D11DepthStencilState* depth_state{nullptr};
        UINT stencil_ref{0};
        ID3D11RasterizerState* rasterizer{nullptr};
        D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
        UINT viewport_count{0};
        D3D11_PRIMITIVE_TOPOLOGY topology{D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED};
    };

    void BeginDraw();
    void EndDraw();

    Microsoft::WRL::ComPtr<ID3D11DeviceContext1> context1_;
    Microsoft::WRL::ComPtr<ID3DDeviceContextState> state_;
    Microsoft::WRL::ComPtr<ID3DDeviceContextState> game_state_;
    Saved saved_;
    std::optional<ScopedD3D11Unpredicated> unpredicated_;
    bool context_states_;
    struct Source {
        ID3D11Texture2D* texture{nullptr};
        std::uint64_t key{0};
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
    };
    // A source the runtime made without shader-resource binding is copied
    // here first.
    struct Copy {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
        std::uint32_t width{0};
        std::uint32_t height{0};
        DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN};
    };
    bool Ensure(std::string& error);
    // The view to read `view`'s rectangle of `source` by, and where the
    // rectangle starts in it: the image itself, or `copy` of the rectangle.
    ID3D11ShaderResourceView* Readable(ID3D11Texture2D* source, const D3D11_TEXTURE2D_DESC& source_desc,
                                       const SubmittedView& view, DXGI_FORMAT view_format, Copy& copy,
                                       std::int32_t& offset_x, std::int32_t& offset_y, std::string& error);

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vertex_shader_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> pixel_shader_;
    // With a focus view drawn over the eye; made with the first such draw.
    Microsoft::WRL::ComPtr<ID3D11PixelShader> focus_pixel_shader_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> constants_;
    std::vector<Source> sources_;
    Copy copy_;
    Copy focus_copy_;
    MirrorSwapChain swapchain_;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> back_buffer_;
    Microsoft::WRL::ComPtr<IDXGIFactory2> factory_; // the device's, found once
    bool ready_{false};
    bool failed_{false};
};

// Direct3D 12: one command list at a time on the session's queue (a frame is
// skipped while the GPU has not finished the last one), with its own fence.
// The source rests in RENDER_TARGET, as do tracked images drawn into.
class D3D12Mirror {
  public:
    D3D12Mirror(ID3D12Device* device, ID3D12CommandQueue* queue);
    // `source_rest`: the state the source rests in (a session's swapchain
    // image: RENDER_TARGET; an image shared with a Vulkan game's device:
    // COMMON). A source made for simultaneous access that rests in COMMON
    // is read with no transition at all (Direct3D 12 promotes it), so the
    // draw is in no other queue's way.
    MirrorDrawResult Draw(ID3D12Resource* source, const SubmittedView& view, const SubmittedFormat& source_format,
                          ID3D12Resource* target, const MirrorDraw& draw, std::string& error,
                          D3D12_RESOURCE_STATES source_rest = D3D12_RESOURCE_STATE_RENDER_TARGET);
    // The GPU is done with every draw so far (and with the window's frame).
    bool Idle() const;
    // An image let go of while the GPU may still read it: kept until it has.
    void Retire(Microsoft::WRL::ComPtr<ID3D12Resource> image);
    // The window's swap chain; waits (bounded) for the last draw into it.
    void Close();
    void Reset();

  private:
    struct SourceView {
        ID3D12Resource* image{nullptr};
        std::uint32_t slice{0};
        DXGI_FORMAT format{DXGI_FORMAT_UNKNOWN};
    };
    // The mirror's own objects (fence, allocator and list, root signature,
    // descriptor heaps, the window's pipeline), made once: on the window's
    // thread where the draw offers it, since the pipeline alone takes
    // milliseconds to make.
    struct Setup {
        std::mutex mutex;
        bool busy{true};
        bool failed{false};
        std::string error;
        Microsoft::WRL::ComPtr<ID3D12Fence> fence;
        Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> source_heap;
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> target_heap;
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commands;
        Microsoft::WRL::ComPtr<ID3D12PipelineState> window_pipeline;
    };
    static void Build(const std::shared_ptr<Setup>& setup, Microsoft::WRL::ComPtr<ID3D12Device> device);
    static Microsoft::WRL::ComPtr<ID3D12PipelineState> MakePipeline(ID3D12Device* device, ID3D12RootSignature* root,
                                                                    DXGI_FORMAT target_format, std::string& error);
    enum class Readiness { kReady, kPending, kFailed };
    Readiness Ensure(const std::function<bool(std::function<void()>)>* executor, std::string& error);
    bool WaitIdle(DWORD milliseconds);
    ID3D12PipelineState* Pipeline(DXGI_FORMAT target_format, std::string& error);

    Microsoft::WRL::ComPtr<ID3D12Device> device_;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue_;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> source_heap_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> target_heap_;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator_;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commands_;
    std::vector<std::pair<int, Microsoft::WRL::ComPtr<ID3D12PipelineState>>> pipelines_;
    std::vector<SourceView> sources_;
    std::vector<std::pair<std::uint64_t, Microsoft::WRL::ComPtr<ID3D12Resource>>> retired_;
    std::shared_ptr<Setup> setup_;
    MirrorSwapChain swapchain_;
    Microsoft::WRL::ComPtr<ID3D12Resource> buffers_[MirrorSwapChain::kBuffers];
    std::uint64_t serial_{0};
    UINT source_step_{0};
    UINT target_step_{0};
    UINT next_source_{0};
    bool ready_{false};
    bool failed_{false};
};

} // namespace depthxr
