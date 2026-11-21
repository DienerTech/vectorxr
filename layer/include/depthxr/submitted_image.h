#pragma once

// Access to the images a game submits with xrEndFrame: which image, array
// slice and rectangle each projection view names, what its format holds, and
// a per-graphics-API object that reads such a rectangle back to the CPU
// without waiting for the GPU (screenshots) or draws it into a desktop
// window (the stabilized mirror: DrawMirror).
//
// The CPU half (formats, view resolution, pixel conversion) has no graphics
// dependency; the GPU half is created from a session's graphics binding chain
// like the OSD's backend (osd_graphics.h).

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <openxr/openxr.h>

#include "depthxr/mirror.h"
struct ID3D11Device;

namespace depthxr {

enum class SubmittedImageApi { kNone, kD3D11, kD3D12, kVulkan };

// What a color format's texels hold, as far as a reader or a shader must know.
enum class SubmittedPixelKind {
    kUnknown, // not a color format this module reads
    kUnorm8,  // four 8-bit channels
    kUnorm10, // 10:10:10:2, red in the low bits
    kFloat16, // four half floats
};

struct SubmittedFormat {
    std::int64_t format{0};
    SubmittedPixelKind kind{SubmittedPixelKind::kUnknown};
    bool srgb{false};     // the format itself is sRGB-encoded (a view of it decodes)
    bool bgra{false};     // blue first in memory
    bool typeless{false}; // `format` names a typeless family, not a view
    // The fully typed format to copy to, resolve with or view the image by:
    // `format` itself unless that is typeless (then the family's UNORM, or its
    // sRGB with the hint given to ClassifyFormat).
    std::int64_t view_format{0};
    std::uint32_t bytes_per_pixel{0};
};

// `format` is a DXGI_FORMAT (Direct3D 11 and 12) or a VkFormat (Vulkan), as
// xrCreateSwapchain was given it. `typeless_is_srgb` chooses the view of a
// typeless Direct3D family.
SubmittedFormat ClassifyFormat(SubmittedImageApi api, std::int64_t format, bool typeless_is_srgb = false);
// Whether a Direct3D image created as `resource_format` (possibly typeless)
// can be copied to or viewed as `view_format`: the same format family.
bool SameDxgiFamily(std::int64_t resource_format, std::int64_t view_format);

// What the layer knows of a swapchain a submitted view names.
struct SubmittedSwapchainState {
    std::uint32_t width{0};
    std::uint32_t height{0};
    std::uint32_t array_size{1};
    std::uint32_t sample_count{1};
    std::int64_t format{0};
    XrSwapchainUsageFlags usage{0};
    // The image the frame shows: the one last released.
    std::uint32_t image_index{0};
    bool has_image{false};
    // That image's release has not reached the runtime yet: the runtime may
    // read (and on Direct3D 12 and Vulkan, change the state of) an image once
    // it has it, so only a held one is read.
    bool held{true};
};
using SubmittedSwapchainLookup = std::function<bool(XrSwapchain, SubmittedSwapchainState&)>;

// One projection view's image. Not `resolved`, `problem` says why and only
// the swapchain, view index, slice and rectangle are the view's own.
struct SubmittedView {
    bool resolved{false};
    std::string problem;
    // Unresolved only because its image's release has reached the runtime
    // (SubmittedSwapchainState::held): a later frame's may be read.
    bool released{false};
    XrSwapchain swapchain{XR_NULL_HANDLE};
    std::uint32_t view_index{0};
    std::uint32_t image_index{0};
    std::uint32_t array_slice{0};
    XrRect2Di rect{};
    std::int64_t format{0};
    std::uint32_t width{0}; // the whole image
    std::uint32_t height{0};
    std::uint32_t array_size{1};
    std::uint32_t sample_count{1};
    XrSwapchainUsageFlags usage{0};
};

// The views of the frame's first projection layer that has any, one entry
// per view. False with `problem` when there is none. A view is left
// unresolved when it names a swapchain the lookup does not know, an image
// never released or already the runtime's, a slice outside the array or a rectangle that is empty or
// not inside the image.
bool ResolveSubmittedViews(const XrFrameEndInfo& frame, const SubmittedSwapchainLookup& lookup,
                           std::vector<SubmittedView>& views, std::string& problem);

// A rectangle of texels as read back: rows of `row_pitch` bytes at `data`.
struct SubmittedPixels {
    const std::uint8_t* data{nullptr};
    std::uint32_t row_pitch{0};
    std::uint32_t width{0};
    std::uint32_t height{0};
    SubmittedFormat format;
};

// To 8-bit RGBA for an image file, `out` holding width * height * 4 bytes:
// 8-bit formats as they are (sRGB or not; blue-first swapped), 10-bit and
// half-float texels clamped to 0..1 and encoded from linear to sRGB, alpha
// 255 throughout. False for a kind it does not read.
bool ConvertToRgba8(const SubmittedPixels& pixels, std::uint8_t* out);
// The 8-bit sRGB encoding of a linear value (clamped to 0..1; NaN is 0).
std::uint8_t LinearToSrgb8(float linear);
float HalfToFloat(std::uint16_t half);

// The focus view of a quad views eye that was not composed into one image
// (the runtime composes the four views, or Frame Generation does): drawn
// over the part of the eye's own view it covers, so the mirror window and
// screenshots show the eye as the headset does and not its low-resolution
// peripheral view alone. Without the compositor's focus sharpening.
struct SubmittedFocus {
    bool present{false};
    SubmittedView view;     // the focus view's image
    MirrorFocusPlace place; // where it lies in the eye's view
};

// One frame of the stabilized mirror window (mirror.h).
struct MirrorDraw {
    // The desktop window (an HWND) whose swap chain is drawn into and
    // presented. It is made once, `width` x `height` (the picture's size:
    // MirrorWindow::OutputSize, not the window's size on the desktop, to
    // which Windows stretches the picture), and made anew only when that
    // size changes: dragging or resizing the window makes nothing.
    void* window{nullptr};
    // With no window: a tracked image of at least that size, 8-bit and not
    // sRGB, drawn into at its top-left corner instead (how tests read the
    // picture back).
    XrSwapchain target{XR_NULL_HANDLE};
    std::uint32_t target_image{0};
    std::uint32_t width{0};
    std::uint32_t height{0};
    MirrorHomography homography{};
    // Drawn over the eye's view where the backend can (SupportsFocus).
    SubmittedFocus focus;
    // Runs a task on the window's own thread (MirrorWindow::Post), where the
    // window's swap chain is made: making one takes tens of milliseconds,
    // which the thread that ends the game's frames must not spend. Until it
    // is there the draw is kSkipped. False (or none given): the task could
    // not be handed over and is run by the caller.
    std::function<bool(std::function<void()>)> on_window_thread;
};

enum class MirrorDrawResult {
    kShown,
    kSkipped, // the window or the GPU is not ready for a frame: none was drawn or shown, nothing waited
    kFailed,
};

class SubmittedImageGpu;

// Tests: every making of a mirror window's swap chain takes this much longer
// (on the thread that makes it), to show that the drawing thread never waits
// for it.
void SetMirrorSwapChainDelayForTests(std::uint32_t milliseconds);
// Tests, a Vulkan session's object: the Direct3D 12 device and queue the
// window is drawn with once they are made (null before), a Direct3D 12
// render target of that device to draw into instead of a window, the GPU
// time of the game queue's last copy (milliseconds; negative: not measured,
// which SetMirrorVulkanTimingForTests turns on), and how many copies ran.
void* MirrorVulkanHostDeviceForTests(SubmittedImageGpu& gpu);
void* MirrorVulkanHostQueueForTests(SubmittedImageGpu& gpu);
void SetMirrorVulkanTargetForTests(SubmittedImageGpu& gpu, void* d3d12_resource);
void SetMirrorVulkanTimingForTests(SubmittedImageGpu& gpu, bool on);
// Tests: every making of a Vulkan mirror's shared images (their import into
// the game's device included) takes this much longer on the thread that
// makes them, to show that the drawing thread never waits for it.
void SetMirrorVulkanImportDelayForTests(std::uint32_t milliseconds);
// Tests: called on the thread that submits a Vulkan mirror's copy to the
// game's queue, with the queue's mutex held, right before the submit.
void SetMirrorVulkanSubmitHookForTests(void (*hook)());
// Tests: the eye is always copied whole, never scaled down for the window.
void SetMirrorVulkanFullCopyForTests(SubmittedImageGpu& gpu, bool on);
double MirrorVulkanCopyGpuMsForTests(SubmittedImageGpu& gpu);
std::uint64_t MirrorVulkanCopiesForTests(SubmittedImageGpu& gpu);
// Tests: how many swap chains were made for mirror windows so far.
std::uint32_t MirrorSwapChainsMadeForTests();

enum class ReadbackState {
    kNone,    // no such ticket
    kPending, // the GPU has not finished the copy
    kReady,   // mapped: the pixels stay valid until Finish
    kFailed,
};

// One session's access to its swapchain images. Every call but the reading of
// a kReady ticket's pixels is made on the thread that ends the game's frames,
// with the game's Direct3D 11 context held where there is one (GameContextLock).
// A Vulkan session's object shows the mirror window only (Vulkan images are
// not read back or processed yet): its calls are made inside the game's
// xrEndFrame, where XR_KHR_vulkan_enable lets the game's queue be used.
class SubmittedImageGpu {
  public:
    virtual ~SubmittedImageGpu() = default;
    virtual const char* Name() const = 0;
    virtual SubmittedImageApi Api() const = 0;
    // Swapchain usage a source image needs beyond the game's own for
    // Readback: none on Direct3D, whose copies need no
    // usage of their own.
    virtual XrSwapchainUsageFlags RequiredSourceUsage() const { return 0; }
    // Swapchain usage a source image must have for DrawMirror: none on
    // Direct3D (the image is sampled, or copied, as it is); a Vulkan image
    // is copied by the game's queue, which needs the transfer source usage.
    virtual XrSwapchainUsageFlags MirrorSourceUsage() const { return 0; }
    // Whether Readback can work at all (screenshots).
    virtual bool SupportsReadback() const { return true; }
    // The swapchain's images as xrEnumerateSwapchainImages gave them to the
    // game (the structures of this object's API). Tracking again replaces.
    virtual bool Track(XrSwapchain swapchain, const XrSwapchainImageBaseHeader* images, std::uint32_t count,
                       std::string& error) = 0;
    virtual void Forget(XrSwapchain swapchain) = 0;
    virtual bool Tracked(XrSwapchain swapchain) const = 0;
    // Starts a copy of the view's rectangle of its array slice (multisampled
    // images resolved) to memory the CPU can read. Never waits for the GPU.
    // Returns a ticket, or 0 with `error`.
    virtual std::uint64_t Readback(const SubmittedView& view, std::string& error) = 0;
    // Whether a focus view can be drawn over its eye (MirrorDraw::focus,
    // ReadbackComposed): Direct3D 11.
    virtual bool SupportsFocus() const { return false; }
    // A readback of a quad views eye with its focus view drawn over it:
    // `view` (the eye's own, peripheral view) and `focus` are drawn into an
    // image of this object's own, `width` x `height` (8-bit, display-encoded
    // as the mirror window's picture), and that is read back as Readback
    // reads a view. One draw on the game's device; never waits for the GPU.
    virtual std::uint64_t ReadbackComposed(const SubmittedView&, const SubmittedFocus&, std::uint32_t, std::uint32_t,
                                           std::string& error) {
        error = "Not supported";
        return 0;
    }
    // Never waits either. kReady fills `pixels`, which any thread may read
    // until Finish.
    virtual ReadbackState Poll(std::uint64_t ticket, SubmittedPixels& pixels, std::string& error) = 0;
    // Ends a ticket in any state and frees its memory. A ticket whose copy
    // the GPU has not finished keeps its resources until it has.
    virtual void Finish(std::uint64_t ticket) = 0;
    // The stabilized mirror window: draws the view's rectangle of its array
    // slice through the homography into the window's swap chain, on the
    // game's device, and presents it. Never waits: not for the display (no
    // vertical sync; a window that cannot take a frame now is kSkipped) and
    // not for the GPU (Direct3D 12 shows a frame only once it has finished
    // the last). The picture holds display-encoded values: linear texels are
    // encoded, samples outside the rectangle are black. Direct3D 11 leaves
    // the game's context as it found it; Direct3D 12 expects the source in
    // RENDER_TARGET and leaves it there. Multisampled sources are refused.
    // Vulkan: the game's queue copies the view's rectangle, scaled down to
    // what the window's picture needs where the format can be blitted (the image in
    // COLOR_ATTACHMENT_OPTIMAL, as XR_KHR_vulkan_enable has it released, and
    // left so) into an image shared with a Direct3D 12 device of VectorXR's
    // own, which draws and presents the window a call later, once the copy
    // is done (MirrorRelay); the game's device gets no window or swap chain.
    virtual bool SupportsMirror() const { return false; }
    virtual MirrorDrawResult DrawMirror(const SubmittedView&, const MirrorDraw&, std::string& error) {
        error = "Not supported";
        return MirrorDrawResult::kFailed;
    }
    // Lets go of the window's swap chain (before the window is destroyed);
    // Direct3D 12 waits, bounded, for its last draw into it.
    virtual void CloseMirror() {}
    // Vulkan: the mutex everything else that uses the game's queue takes
    // around its calls on it (a VkQueue is the caller's to synchronize); the
    // mirror's copies are submitted under it. None: not locked.
    virtual void SetQueueMutex(std::mutex*) {}
    // The session's device may be gone (a session whose end was never
    // seen): everything is forgotten, and where the device is not kept alive
    // by what is held of it (Vulkan) no call is made on it. Nothing but the
    // destructor is called after.
    virtual void Abandon() { Reset(); }
    // Teardown: may wait for outstanding copies, then forgets everything.
    virtual void Reset() = 0;
};

// From a session's graphics binding chain (XrSessionCreateInfo::next). Null
// with `error` for an API without a backend (Vulkan so far).
std::unique_ptr<SubmittedImageGpu> CreateSubmittedImageGpu(const void* binding_chain, std::string& error);
// What that reads of the chain, to make the object later in the session: the
// game's device (and Direct3D 12 queue) by address, with no reference held
// (the game keeps them for as long as its session lives).
struct SubmittedImageBinding {
    XrStructureType type{XR_TYPE_UNKNOWN}; // the graphics binding's, or none found
    void* device{nullptr};
    void* queue{nullptr};
    // Vulkan (`device` is the VkDevice): the rest of the binding, and the
    // game's vkGetInstanceProcAddr where the layer knows it
    // (XR_KHR_vulkan_enable2; null: the Vulkan loader the game loaded).
    void* vulkan_instance{nullptr};
    void* vulkan_physical_device{nullptr};
    std::uint32_t vulkan_queue_family{0};
    std::uint32_t vulkan_queue_index{0};
    PFN_xrVoidFunction vulkan_get_instance_proc_addr{nullptr};
};
SubmittedImageBinding CaptureSubmittedImageBinding(const void* binding_chain);
std::unique_ptr<SubmittedImageGpu> CreateSubmittedImageGpu(const SubmittedImageBinding& binding, std::string& error);
std::unique_ptr<SubmittedImageGpu> CreateSubmittedImageD3D11(ID3D11Device* device, bool context_states = true);
// Tracks a swapchain from the runtime's own enumeration (`enumerate` is the
// next xrEnumerateSwapchainImages), for a caller without the API's image
// structures. Not for a bridged swapchain, whose game-facing images are not
// the runtime's.
bool TrackSubmittedSwapchain(
    SubmittedImageGpu& gpu, XrSwapchain swapchain,
    const std::function<XrResult(XrSwapchain, std::uint32_t, std::uint32_t*, XrSwapchainImageBaseHeader*)>& enumerate,
    std::string& error);
// For a Vulkan game on Frame Generation's Vulkan bridge, whose session with
// the runtime is Direct3D 12: the mirror window only, drawn on the bridge's
// device and queue (`binding_chain`: the bridge's Direct3D 12 binding) from
// the Direct3D 12 side of the images the game renders into
// (VulkanBridge::SharedImages, tracked with TrackSubmittedSwapchainD3D12).
// Those are made for simultaneous access and rest in COMMON: the draw reads
// them with no state transition, behind the bridge's own copy on that
// queue, so it never touches the runtime's images, which Frame Generation's
// queue uses at the same time. Screenshots stay
// unavailable, as for every Vulkan game.
std::unique_ptr<SubmittedImageGpu> CreateSubmittedImageMirrorOnly(const void* binding_chain, std::string& error);
// Tracks a swapchain whose Direct3D 12 textures (ID3D12Resource pointers)
// the caller holds.
bool TrackSubmittedSwapchainD3D12(SubmittedImageGpu& gpu, XrSwapchain swapchain, void* const* resources,
                                  std::uint32_t count, std::string& error);
// Tracks a swapchain whose Direct3D 11 textures the caller holds.
bool TrackSubmittedSwapchainD3D11(SubmittedImageGpu& gpu, XrSwapchain swapchain, void* const* textures,
                                  std::uint32_t count, std::string& error);

} // namespace depthxr
