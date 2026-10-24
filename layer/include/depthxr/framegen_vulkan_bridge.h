#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <openxr/openxr.h>

namespace depthxr {

// What the layer knows about the game's Vulkan device (XR_KHR_vulkan_enable2's
// hooks record it, vulkan_enable_hooks.h).
struct VulkanBridgeGame {
    // The game's vkGetInstanceProcAddr; null for the loader the game loaded.
    PFN_xrVoidFunction get_instance_proc_addr{nullptr};
    // The game's device was made with timeline semaphores enabled: the
    // bridge's fences are then imported as timeline semaphores (otherwise as
    // binary ones, with VkD3D12FenceSubmitInfoKHR values).
    bool timeline_semaphores{false};
};

// Vulkan games on a Direct3D 12 session, for Frame Generation.
//
// Frame Generation's full path (a presenter thread of its own, frames written
// just in time on a queue of its own, NVIDIA's optical flow) is Direct3D 12
// only, and a Vulkan game's queue is the game's alone: nothing but the
// game's own OpenXR calls may submit to it. A Vulkan game gets the full path
// through the bridge: the runtime's session is Direct3D 12, on a device made
// on the game's adapter, and the game keeps rendering with Vulkan into images
// both APIs share.
//
// Each image of a runtime swapchain gets a twin, made by Direct3D 12 as a
// shared resource and imported into the game's Vulkan device. The game renders
// into the twin; when the image is released, the twin is handed to Direct3D 12
// (a queue family ownership release to the external queue family) and copied
// into the runtime's image on the bridge's queue, which the runtime queue
// substitution and Frame Generation then take for the game's queue (Frame
// Generation captures the runtime's image after the copy, as it does for a
// Direct3D 12 game). Two shared fences, imported into Vulkan as semaphores,
// keep the APIs in order: "written", signalled by the game's queue in the
// bridge's release submission and waited for by the bridge's queue before
// the copy, and "done", signalled by the bridge's queue after the copy and
// waited for by the game's queue in the bridge's acquire submission. Neither
// API waits for the other on the CPU.
//
// Depth is a twin as well (the runtime gets the game's depth). Multisampled
// colour is rendered privately and resolved into the twin at release;
// multisampled depth stays private and never reaches the runtime (the caller
// keeps it out of submitted frames).
//
// Every Vulkan call that uses the game's queue is made inside the
// application's own xrAcquireSwapchainImage, xrReleaseSwapchainImage and
// xrEndFrame (where the layer hands releases over), the calls in which
// XR_KHR_vulkan_enable lets a runtime use it; nothing here is called from the
// presenter thread.
class VulkanBridge {
  public:
    VulkanBridge();
    ~VulkanBridge();
    VulkanBridge(const VulkanBridge&) = delete;
    VulkanBridge& operator=(const VulkanBridge&) = delete;

    // Where retirements that leaked, skipped copies and refused submissions are reported.
    void SetLog(std::function<void(const std::string&)> log);
    // Where what is only worth knowing is reported (the game's frame calls
    // from several threads, a depth swapchain kept private); without it,
    // the log above.
    void SetInfoLog(std::function<void(const std::string&)> log);

    // The Vulkan extensions the bridge needs on the game's instance and device:
    // XR_KHR_vulkan_enable's extension lists and XR_KHR_vulkan_enable2's create
    // infos get the missing ones added.
    static const std::vector<const char*>& InstanceExtensions();
    static const std::vector<const char*>& DeviceExtensions();
    // `list` (space-separated, as XR_KHR_vulkan_enable returns it) with the
    // names of `extra` it lacks appended, in order.
    static std::string WithExtensions(const std::string& list, const std::vector<const char*>& extra);

    // Before xrCreateSession, for a session whose first structure is a Vulkan
    // binding (XR_KHR_vulkan_enable or _enable2): makes a Direct3D 12 device on
    // the game's adapter (which must be the one the runtime asks for), the
    // bridge's queue and the shared fences, imported into the game's device.
    // `vulkan_get_instance_proc_addr` is the game's vkGetInstanceProcAddr (from
    // XR_KHR_vulkan_enable2's create infos), or null for the loader the game
    // loaded. Every Vulkan function the bridge calls (vkGetDeviceProcAddr
    // included) is resolved through it. Returns create info naming a
    // Direct3D 12 binding of them, with the application's other structures
    // after it (valid until End), or nullptr with `reason`. `description` says
    // what was made.
    const XrSessionCreateInfo* Begin(XrInstance instance, const XrSessionCreateInfo* info,
                                     PFN_xrGetInstanceProcAddr get_instance_proc_addr, const VulkanBridgeGame& game,
                                     std::string& reason, std::string& description);
    // Whether the last Begin asked the runtime for its Direct3D 12 graphics
    // requirements: a session made with Vulkan after all should ask for the
    // Vulkan ones again.
    bool QueriedD3D12Requirements() const;
    bool Active() const;
    // The Direct3D 12 binding (the bridge's device and queue), which Frame
    // Generation's backend is made from.
    const void* Binding() const;
    // After xrCreateSession: the application's structures after the binding
    // are not followed any more.
    void SessionCreated();
    // At session (or instance) teardown, before Frame Generation stops: lets
    // the bridge's queue get past the game's last release (bounded); when the
    // game's device is lost or the wait times out, "written" is signalled from
    // the CPU, so Frame Generation's own waits on the bridge's queue end.
    bool Escape();
    // Before the runtime destroys the session (or the instance), while its
    // images still exist: waits for the bridge's queue (bounded), past Vulkan
    // work that never ends if need be. False when the queue still did not
    // finish.
    bool Drain();
    // After xrDestroySession, a failed xrCreateSession or xrDestroyInstance:
    // lets everything go. Work either queue still holds keeps what it uses for
    // the process, and Vulkan stops waiting for the bridge's queue.
    void End();
    // The bridge's device (the session's) was removed, or the game's Vulkan
    // device was lost: the session is lost.
    bool Lost(std::string& reason) const;
    // At each xrEndFrame (the application's thread): notes the threads the
    // bridge's Vulkan calls come from (a second one is reported once). With
    // VECTORXR_VULKAN_BRIDGE_DEBUG=1, also reports what the Direct3D 12 debug
    // layer found.
    void FrameEnded();
    // Research, log only (VECTORXR_FRAMEGEN_DEPTH_BRIDGE_PROBE=1, read with
    // Frame Generation's research switches at its Start): about once a second
    // each depth swapchain's hand-over reads a grid of its twin on the game's
    // queue just before the release, and its runtime image on the bridge's
    // queue just after the copy, and logs both (nonzero texels, least and
    // most). Off, nothing of it is made or submitted.
    void SetDepthProbe(bool on);

    // Swapchain formats: a VkFormat's DXGI format for the runtime, and back
    // (0 when there is none).
    static std::int64_t RuntimeFormat(std::int64_t vulkan_format);
    static std::int64_t GameFormat(std::int64_t dxgi_format);
    // The runtime's formats (DXGI, in its order of preference) in Vulkan's
    // terms, without the ones Vulkan has no format for (or repeats).
    static std::vector<std::int64_t> TranslatedFormats(const std::vector<std::int64_t>& runtime_formats);
    // What the game is offered on a bridged session: the translated formats the
    // game's device can import as the bridge's images.
    std::vector<std::int64_t> GameFormats(const std::vector<std::int64_t>& runtime_formats) const;
    // Before the runtime is asked for a swapchain: whether the bridge can make
    // the images the application's create info needs. False with the result
    // the application gets (XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED or
    // XR_ERROR_FEATURE_UNSUPPORTED) and `reason`.
    bool CheckSwapchain(const XrSwapchainCreateInfo& info, XrResult& refusal, std::string& reason) const;
    // The runtime's create info for the application's: the DXGI format; the
    // bridge's copies write the runtime's images (transfer destination; colour
    // images as render targets, depth images as depth targets); multisampled
    // colour is resolved to one
    // sample; Vulkan-only structures leading the chain are left out. False
    // with `reason` when the format has no DXGI equivalent.
    static bool RuntimeCreateInfo(const XrSwapchainCreateInfo& info, XrSwapchainCreateInfo& runtime,
                                  std::string& reason);
    // After the runtime created `swapchain` from RuntimeCreateInfo(info):
    // makes an image for each of its images (`enumerate` is the runtime's).
    // `info` is the application's create info.
    bool AddSwapchain(XrSwapchain swapchain, const XrSwapchainCreateInfo& info,
                      PFN_xrEnumerateSwapchainImages enumerate, std::string& reason);
    bool Bridged(XrSwapchain swapchain) const;
    // Bridged, and the runtime's images never get anything (multisampled
    // depth): the runtime's calls on it need no ordering with the bridge's
    // queue, and its depth stays out of submitted frames.
    bool Unmirrored(XrSwapchain swapchain) const;
    bool HasUnmirrored() const;
    // The VkImage handles the game renders into, by image index (empty when not bridged).
    std::vector<std::uint64_t> Images(XrSwapchain swapchain) const;
    // The application's xrEnumerateSwapchainImages of a bridged swapchain
    // (XrSwapchainImageVulkanKHR structures), with the two-call idiom.
    XrResult EnumerateImages(XrSwapchain swapchain, std::uint32_t capacity, std::uint32_t* count,
                             XrSwapchainImageBaseHeader* images) const;
    // After the runtime handed out image `index` (inside the application's
    // xrAcquireSwapchainImage): the game's queue takes the image back from
    // Direct3D 12, waiting (on the GPU) until the bridge's queue is done with
    // its last release.
    void Acquired(XrSwapchain swapchain, std::uint32_t index);
    // At the frame's end after the application released the image (inside
    // its xrEndFrame, or its next acquire on the swapchain: the layer hands
    // releases over where a runtime reads the image, as a game may render
    // into it after releasing it), before the release goes on (and before
    // Frame Generation captures the runtime's image): the
    // oldest acquired image is handed to Direct3D 12 and copied into the
    // runtime's image on the bridge's queue. False when the copy was skipped
    // (the bridge's queue is stalled, or the game's queue refused the
    // submission): the runtime's image holds an older frame, which Frame
    // Generation must not capture as new. `teardown`: at the session's end,
    // the image is not handed back to the game's queue (nothing renders into
    // it again; a hand-back would make the game's queue wait for the copy).
    bool Released(XrSwapchain swapchain, bool teardown = false);
    // The game's motion vectors (XR_FB_space_warp, XR_EXT_frame_synthesis)
    // are in `swapchain`'s images, which Frame Generation reads in the
    // runtime's images after each copy: from now on each release gives the
    // image back to the game's queue at once, behind its copy, as depth's
    // releases do (a game that renders into an image before acquiring it,
    // Star Citizen, would otherwise write the next frame's vectors while the
    // copy reads this one's). Colour twins only: false for any other.
    bool MarkMotion(XrSwapchain swapchain);
    // Undoes MarkMotion while Frame Generation reads the vectors no more
    // (Star Citizen made a swapchain within the settle time, the validator
    // disabled them, the session reads none): releases give nothing back
    // early and nothing arms the device-lost sentinel; an image given back
    // already needs nothing at its next acquire. False when the swapchain was
    // not marked.
    bool UnmarkMotion(XrSwapchain swapchain);
    // For a game that writes its motion vector images without acquiring or
    // releasing them (stage 9's contingency), inside its xrEndFrame: each of
    // `images` (an image of a swapchain MarkMotion named that the game never
    // acquired), as the game left it (in the attachment layout, on its queue
    // family), is handed to Direct3D 12 (one submission for them all on the
    // game's queue, signalling "written"), copied into its runtime image on
    // the bridge's queue and given back to the game's queue behind the copies
    // (one more submission). An image the bridge never gave the game is not
    // copied the first time: it is put in the attachment layout (its content
    // undefined), and one whose giving back was refused is given back. Never
    // waits for the bridge's queue. Per image whether it was copied: Frame
    // Generation reads the runtime's image then, on the bridge's queue.
    std::vector<bool> EndFrameMotion(const std::vector<std::pair<XrSwapchain, std::uint32_t>>& images);
    // Before the runtime destroys `swapchain`: waits for the bridge's copies
    // into it and the bridge's submissions on the game's queue (bounded) and
    // lets its images go. False when they did not finish: the runtime's
    // swapchain must then be kept until the session ends.
    bool RemoveSwapchain(XrSwapchain swapchain);
    // Waits for everything queued on the bridge's queue so far (bounded).
    bool WaitIdle(std::uint32_t timeout_ms);

    std::string Describe(XrSwapchain swapchain) const;
    std::string Summary() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::function<void(const std::string&)> log_;
    std::function<void(const std::string&)> info_log_;
    bool queried_d3d12_requirements_{false};
};

} // namespace depthxr
