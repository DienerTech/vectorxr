#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <openxr/openxr.h>

struct ID3D11Texture2D;

namespace depthxr {

// Direct3D 11 games on a Direct3D 12 session, for Frame Generation.
//
// Frame Generation's full path (a presenter thread of its own, frames written
// just in time on a queue of its own, NVIDIA's optical flow) is Direct3D 12
// only. A Direct3D 11 game gets it through the bridge: the runtime's session
// is Direct3D 12, on a device made on the game's adapter, and the game keeps
// rendering with Direct3D 11 into images both APIs share.
//
// Each image of a runtime swapchain gets a twin, made by Direct3D 12 with
// simultaneous access and opened on the game's device. The game (and the
// layer's own Direct3D 11 rendering) renders into the twin; when the image is
// released, the twin is copied into the runtime's image on the bridge's queue,
// which the runtime queue substitution and Frame Generation then take for the
// game's queue (Frame Generation captures the runtime's image after the copy,
// as it does for a Direct3D 12 game). Two shared fences keep the APIs in
// order: "written", signalled by Direct3D 11 at each release and waited for
// by the bridge's queue before the copy, and "done", signalled by the bridge's
// queue after the copy and waited for by Direct3D 11 when the image is
// acquired again. Neither API waits for the other on the CPU.
//
// Images a twin cannot be: multisampled ones are rendered privately and
// resolved into the twin at release; depth stays private and never reaches
// the runtime (the caller keeps it out of submitted frames).
//
// Every Direct3D 11 call is made inside the application's own OpenXR calls,
// on its thread; nothing here is called from the presenter thread.
class D3D11Bridge {
  public:
    D3D11Bridge();
    ~D3D11Bridge();
    D3D11Bridge(const D3D11Bridge&) = delete;
    D3D11Bridge& operator=(const D3D11Bridge&) = delete;

    // Where retirements that leaked, skipped copies and second threads are reported.
    void SetLog(std::function<void(const std::string&)> log);

    // Before xrCreateSession, for a session whose first structure is a
    // Direct3D 11 binding: makes a Direct3D 12 device on the game's adapter
    // (which must be the one the runtime asks for), the bridge's queue and the
    // shared fences. Returns create info naming a Direct3D 12 binding of them,
    // with the application's other structures after it (valid until End), or
    // nullptr with `reason`. `description` says what was made.
    const XrSessionCreateInfo* Begin(XrInstance instance, const XrSessionCreateInfo* info,
                                     PFN_xrGetInstanceProcAddr get_instance_proc_addr, std::string& reason,
                                     std::string& description);
    // Whether the last Begin asked the runtime for its Direct3D 12 graphics
    // requirements: a session made with Direct3D 11 after all should ask for
    // those again.
    bool QueriedD3D12Requirements() const;
    bool Active() const;
    // The Direct3D 12 binding (the bridge's device and queue), which Frame
    // Generation's backend is made from.
    const void* Binding() const;
    // After xrCreateSession: the application's structures after the binding
    // are not followed any more.
    void SessionCreated();
    // Before the runtime destroys the session (or the instance), while its
    // images still exist: waits for the bridge's queue (bounded), past
    // Direct3D 11 work that never ends if need be. False when the queue
    // still did not finish.
    bool Drain();
    // After xrDestroySession, a failed xrCreateSession or xrDestroyInstance:
    // lets everything go. Work the bridge's queue still holds keeps what it
    // uses (the runtime's images included) for the process, and Direct3D 11
    // stops waiting for it.
    void End();
    // The bridge's device (the session's) was removed: the session is lost.
    bool Lost(std::string& reason) const;
    // At each xrEndFrame (the application's thread): the game's immediate
    // context is used by the bridge from the threads its swapchain calls come
    // from; when those (or the frame's) differ, the device's multithread
    // protection is turned on, so the bridge's calls cannot race the game's.
    // With VECTORXR_D3D11_BRIDGE_DEBUG=1, also reports what the Direct3D 12
    // debug layer found.
    void FrameEnded();

    // The runtime's create info for the application's: the bridge's copies
    // write the runtime's colour images as render targets do, and
    // multisampled images are resolved to one sample.
    static XrSwapchainCreateInfo RuntimeCreateInfo(const XrSwapchainCreateInfo& info);
    // After the runtime created `swapchain` from RuntimeCreateInfo(info):
    // makes an image for each of its images (`enumerate` is the runtime's).
    bool AddSwapchain(XrSwapchain swapchain, const XrSwapchainCreateInfo& info,
                      PFN_xrEnumerateSwapchainImages enumerate, std::string& reason);
    bool Bridged(XrSwapchain swapchain) const;
    // Whether the layer composes the swapchain's images into images of its
    // own (Quadviews' views), so the runtime shows them only when composing
    // fails: their releases need not wait on the runtime's queue.
    void SetComposed(XrSwapchain swapchain, bool composed);
    bool Composed(XrSwapchain swapchain) const;
    // Bridged, and the runtime's images never get anything (depth): the
    // runtime's calls on it need no ordering with the bridge's queue, and its
    // depth stays out of submitted frames.
    bool Unmirrored(XrSwapchain swapchain) const;
    bool HasUnmirrored() const;
    // The Direct3D 11 textures rendered into, by image index (empty when not bridged).
    std::vector<ID3D11Texture2D*> Images(XrSwapchain swapchain) const;
    // After the runtime handed out image `index`: Direct3D 11 waits (on the
    // GPU) until the bridge's queue is done with the image's last release.
    void Acquired(XrSwapchain swapchain, std::uint32_t index);
    // At the frame's end after the application released the image (the layer
    // hands releases over inside xrEndFrame, or the next acquire on the
    // swapchain, as a runtime reads it), before the release goes on (and
    // before Frame Generation captures the
    // runtime's image): the oldest acquired image is handed to Direct3D 12 and
    // copied into the runtime's image on the bridge's queue. False when the
    // copy was skipped (the bridge's queue is stalled): the runtime's image
    // holds an older frame, which Frame Generation must not capture as new.
    bool Released(XrSwapchain swapchain);
    // The application's xrReleaseSwapchainImage of a bridged image, whose
    // hand-over (Released) waits for the frame's end: its thread counts among
    // the game's swapchain and frame calls' threads, as it did when the copy
    // ran there (a second thread turns multithread protection on).
    void ReleaseCalled();
    // A release of an image nothing was drawn into: no copy; the runtime's
    // image keeps what it had.
    void Abandoned(XrSwapchain swapchain);
    // Before the runtime destroys `swapchain`: waits for the bridge's copies
    // into it (bounded) and lets its images go. False when they did not
    // finish: the runtime's swapchain must then be kept until the session ends.
    bool RemoveSwapchain(XrSwapchain swapchain);
    // Waits for everything queued on the bridge's queue so far (bounded).
    bool WaitIdle(std::uint32_t timeout_ms);

    std::string Describe(XrSwapchain swapchain) const;
    std::string Summary() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::function<void(const std::string&)> log_;
    bool queried_d3d12_requirements_{false};
};

} // namespace depthxr
