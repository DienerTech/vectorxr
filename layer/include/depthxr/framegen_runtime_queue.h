#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include <openxr/openxr.h>

namespace depthxr {

// Direct3D 12 queue substitution for Frame Generation.
//
// A runtime synchronizes every swapchain image released to it with the queue
// the session was created with: the image counts as written when everything
// submitted to that queue before the release has run. Frame Generation writes
// generated frames right before their refreshes, on a high-priority queue of
// its own; released through the game's queue, a frame would only count as
// written once the game's GPU work queued ahead of it had run, often most of
// a game frame later. So the runtime is given Frame Generation's queue
// instead, and the layer keeps the game's images in order across the two:
// before a game image is released to the runtime, the runtime's queue waits
// for the game's work so far; after the runtime hands one back, the game's
// queue waits for anything the runtime queued for it.
//
// That wait on the runtime's queue would hold every frame Frame Generation
// writes after it until the game's GPU work on the image had run, so while
// Frame Generation shows only images of its own, releases of the images it
// captures are lazy instead: the game's queue marks the image's work, and
// the release reaches the runtime once that mark has passed on the GPU (no
// queue waits for it). The runtime must see each swapchain's calls in the
// application's order (a wait only after the image waited before it was
// released), so the application's wait on its next image is held back with
// that release when the release is still pending: the application gets its
// image at once (the runtime shows none of the swapchain's images, and every
// image was waited for once since it was last shown), and the wait reaches
// the runtime right after the release. Held-back calls go at the
// application's next call on the swapchain once their work has run, as far
// as the runtime needs them to hand out an image, or before a frame showing
// the swapchain is submitted (then with the runtime's queue waiting).
class FrameGenQueueSubstitution {
  public:
    // The runtime's swapchain calls (the next layer down).
    struct Calls {
        PFN_xrAcquireSwapchainImage acquire{};
        PFN_xrWaitSwapchainImage wait{};
        PFN_xrReleaseSwapchainImage release{};
    };

    FrameGenQueueSubstitution();
    ~FrameGenQueueSubstitution();
    FrameGenQueueSubstitution(const FrameGenQueueSubstitution&) = delete;
    FrameGenQueueSubstitution& operator=(const FrameGenQueueSubstitution&) = delete;

    // Before xrCreateSession: when `info`'s first structure is a Direct3D 12
    // binding, makes a high-priority queue on its device and returns create
    // info naming it (valid until Reset); otherwise `info` itself, with
    // `note` saying why. VECTORXR_FRAMEGEN_QUEUE=game keeps the game's queue.
    const XrSessionCreateInfo* Prepare(const XrSessionCreateInfo* info, std::string& note);
    bool Active() const;
    // Where a held-back call the runtime failed is reported (a few times).
    void SetLog(std::function<void(const std::string&)> log);
    // The chain Frame Generation's backend is created from: the runtime's
    // queue (a FrameGenRuntimeQueue) ahead of the game's own `game_chain`.
    const void* FrameGenChain(const void* game_chain);
    // The binding chain naming the runtime's queue, for the layer's own
    // rendering into its own swapchains (the OSD).
    const void* RuntimeChain() const;

    // The application's swapchain calls, replacing forwarding them
    // (thread-safe; without substitution they are forwarded as they are).
    // `lazy`: the swapchain's images are captured by Frame Generation, which
    // shows images of its own only.
    // Acquire: held-back calls whose work has run go first; while the
    // runtime would have no image left to hand out, the oldest held-back
    // release is waited for on the CPU (for a while).
    XrResult Acquire(XrSwapchain swapchain, std::uint32_t image_count, const Calls& next,
                     const XrSwapchainImageAcquireInfo* info, std::uint32_t* index);
    // Wait: held back (XR_SUCCESS at once) while a lazy release before it is,
    // unless an image of the swapchain was shown since the swapchain's
    // images were last waited for one by one; otherwise every held-back call
    // goes first.
    XrResult Wait(XrSwapchain swapchain, bool lazy, const Calls& next, const XrSwapchainImageWaitInfo* info);
    // Release: `lazy` holds it back (XR_SUCCESS); otherwise held-back calls
    // go first, then this release, each release after the runtime's queue
    // waits for the game's work.
    XrResult Release(XrSwapchain swapchain, bool lazy, const Calls& next, const XrSwapchainImageReleaseInfo* info);
    // Before a frame showing `swapchain`'s images is submitted, or before it
    // is destroyed: every held-back release goes (after the runtime's queue
    // waits for the game's work), and the swapchain's next waits are not
    // held back until each image was waited for again.
    void Flush(XrSwapchain swapchain, const Calls& next);
    void Forget(XrSwapchain swapchain);
    // Before a release of `swapchain` made elsewhere (deferred releases):
    // held-back calls go first, and the runtime's queue waits for the game's
    // work so far.
    void BeforeRelease(XrSwapchain swapchain, const Calls& next);
    // After xrDestroySession (or a failed xrCreateSession): waits for the
    // runtime's queue and lets everything go.
    void Reset();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::function<void(const std::string&)> log_;
};

} // namespace depthxr
