#pragma once

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
// for the game's work so far (BeforeRelease); after the runtime hands one back,
// the game's queue waits for anything the runtime queued for it (AfterWait).
class FrameGenQueueSubstitution {
  public:
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
    // The chain Frame Generation's backend is created from: the runtime's
    // queue (a FrameGenRuntimeQueue) ahead of the game's own `game_chain`.
    const void* FrameGenChain(const void* game_chain);
    // The binding chain naming the runtime's queue, for the layer's own
    // rendering into its own swapchains (the OSD).
    const void* RuntimeChain() const;
    // The application's swapchain calls (Active only; thread-safe).
    void BeforeRelease();
    void AfterWait();
    // After xrDestroySession (or a failed xrCreateSession): waits for the
    // runtime's queue and lets everything go.
    void Reset();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace depthxr
