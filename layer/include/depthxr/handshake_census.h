#pragma once

// The handshake census, a research aid beside the space-warp probe
// (spacewarp_probe.h), log-only: while VECTORXR_SPACEWARP_PROBE is set, the
// layer logs what the application asks the runtime about, each distinct item
// once, under "Handshake census:" (the instance it created and the extensions
// it enabled against those the runtime lists, every name it looks up with
// xrGetInstanceProcAddr, its system, view configuration, blend mode,
// reference space, refresh rate and swapchain format queries and their
// answers, its session's creation and beginning, the swapchains its frames
// use for color, depth, space-warp depth and motion vectors, and every 30
// seconds how often it acquired, waited for and released the motion vector
// images). With the variable unset nothing is noted, nothing changes and no
// line is logged.
//
// An API layer never sees xrEnumerateInstanceExtensionProperties: the loader
// answers it from the runtime and the API layers' manifests. The census logs
// the runtime's list as the layer reads it before the instance instead.

#include <openxr/openxr.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace depthxr {

// Names for the census's lines: an enum value's name without its type's
// prefix ("SYSTEM_SPACE_WARP_PROPERTIES_FB"), or its number when it has none.
std::string CensusStructureType(XrStructureType type);
// A next chain's structure types, "[A,B]" ("[]" without one).
std::string CensusChain(const void* next);
std::string CensusResult(XrResult result); // "XR_SUCCESS", or the number
std::string CensusFormFactor(XrFormFactor form_factor);
std::string CensusViewConfigurationType(XrViewConfigurationType type);
std::string CensusBlendMode(XrEnvironmentBlendMode mode);
std::string CensusReferenceSpace(XrReferenceSpaceType type);
std::string CensusVersion(XrVersion version); // "1.0.34"
// A swapchain format's number, with its DXGI name where one is known and
// `dxgi` says the session's formats are DXGI's ("10:R16G16B16A16_FLOAT").
std::string CensusSwapchainFormat(std::int64_t format, bool dxgi);
// A fixed-size string field, quoted.
std::string CensusQuoted(const char* text, std::size_t capacity);
// Names, space-separated, in brackets with their count: "(2)[A B]".
std::string CensusList(const std::vector<std::string>& names);

// xrCreateInstance as the application called it: its application and engine
// names and versions, API version, flags, next chain, API layers and
// extensions.
std::string DescribeInstanceRequest(const XrInstanceCreateInfo& info);
// The application's extensions against the runtime's list before the
// instance (`scan_detail` says whether it was read), and what the layer sent
// on: what it took out, and what it added.
std::string DescribeInstanceExtensions(const std::vector<std::string>& requested, bool scan_complete,
                                       std::string_view scan_detail, const std::vector<std::string>& runtime_lists,
                                       const std::vector<std::string>& sent);
// An xrGetSystemProperties call as the application got it back: its chain,
// the system's properties and the chained structures the census knows.
std::string DescribeSystemProperties(XrSystemId system_id, const XrSystemProperties* properties, XrResult result);
// An xrEnumerateViewConfigurationViews call (its populating call) as the
// application got it back.
std::string DescribeViewConfigurationViews(XrViewConfigurationType type, std::uint32_t capacity,
                                           const std::uint32_t* count, const XrViewConfigurationView* views,
                                           XrResult result);

// A swapchain a frame used, by its role ("motionVectors", "spaceWarpDepth",
// "color", "depth"), with the application's calls on it so far.
struct CensusSwapchain {
    XrSwapchain handle{XR_NULL_HANDLE};
    std::string role;
    std::string description; // size, format, usage
    std::uint32_t images{0};
    std::uint64_t acquires{0};
    std::uint64_t waits{0};
    std::uint64_t releases{0};
    bool destroyed{false};
};
// "role 0x..(description) images=3 acquires=0 waits=0 releases=0" for each,
// "; "-separated ("none" without one).
std::string DescribeCensusSwapchains(const std::vector<CensusSwapchain>& swapchains);

// What the census has seen for one instance. Its own lock: the application
// looks names up on any thread, outside the layer's.
class HandshakeCensus {
  public:
    // A new instance: noted from here while `active` (the probe's variable set).
    void Reset(bool active);
    bool Active() const { return active_.load(std::memory_order_acquire); }

    // Counts a call of `function`. Its number the first time `detail` is seen
    // for it (each distinct item is logged once); with `always`, the first
    // 16 calls whatever their detail. None otherwise, or while inactive.
    std::optional<std::uint64_t> Note(std::string_view function, const std::string& detail, bool always = false);
    std::uint64_t Calls(std::string_view function) const;

    // The application's first call of a function the census watches since
    // the instance: true once. The names looked up before it are the
    // loader's (its dispatch table, built inside xrCreateInstance) and the
    // application's first lookups.
    bool FirstApplicationCall();

    // A name looked up with xrGetInstanceProcAddr: kept for the next flush
    // the first time, counted every time.
    void NoteProcAddr(std::string_view name, XrResult result, bool resolved);
    // The names kept since the last flush, as one line ("" without any):
    // "xrGetInstanceProcAddr, N new name(s) <when>: name ... name=unresolved(result)".
    std::string FlushProcAddrs(std::string_view when);
    // The names looked up more than once and how often ("" without any).
    std::string ProcAddrRepeats() const;

  private:
    struct LookedUp {
        std::uint64_t queries{0};
        bool resolved{false};
        XrResult result{XR_SUCCESS};
    };

    std::atomic<bool> active_{false};
    mutable std::mutex mutex_;
    std::unordered_map<std::string, std::uint64_t> calls_;
    std::unordered_map<std::string, std::uint32_t> always_logged_;
    std::unordered_set<std::string> seen_;
    std::unordered_map<std::string, LookedUp> names_;
    std::vector<std::string> order_;   // every name, in the order first looked up
    std::vector<std::string> pending_; // looked up since the last flush
    bool application_called_{false};
};

} // namespace depthxr
