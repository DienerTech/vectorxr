#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace depthxr {

// The graphics hardware a session runs on, logged once when the session is
// made: one line per graphics API in play (the game's, and the Direct3D 12
// device of a Frame Generation bridge), so every log names the adapter, its
// driver and its video memory. Whatever could not be found stays empty.
struct SessionGraphics {
    // "Direct3D 11", "Direct3D 12" or "Vulkan", with what the device is for
    // when it is not the game's (a bridge's).
    std::string api;
    // DXGI's description of the adapter (Vulkan's deviceName when DXGI's
    // adapter was not found).
    std::string adapter;
    std::optional<std::uint32_t> vendor_id; // PCI vendor ID
    // DXGI's user-mode driver version (IDXGIAdapter::CheckInterfaceSupport):
    // a.b.c.d in four 16-bit fields, a in the highest.
    std::optional<std::uint64_t> driver_version;
    std::optional<std::uint32_t> vulkan_driver_version; // VkPhysicalDeviceProperties::driverVersion
    std::optional<std::uint64_t> dedicated_video_memory; // DXGI_ADAPTER_DESC::DedicatedVideoMemory, bytes
    // DXGI_MEMORY_SEGMENT_GROUP_LOCAL of IDXGIAdapter3::QueryVideoMemoryInfo:
    // the process's budget and what it uses of it, bytes.
    std::optional<std::uint64_t> budget;
    std::optional<std::uint64_t> usage;
    // Why something is missing, or how DXGI's adapter was found when not as
    // usual (a clause, for the end of the line).
    std::string problem;
};

// An adapter's PCI vendor and device ID, as DXGI_ADAPTER_DESC1 and
// VkPhysicalDeviceProperties both name them.
struct PciIds {
    std::uint32_t vendor_id{0};
    std::uint32_t device_id{0};
};

// DXGI's adapters with a Vulkan device's PCI IDs, for a device whose LUID is
// not known: the first of them in DXGI's order, and how many there are.
struct AdapterIdMatch {
    std::optional<std::size_t> first;
    std::size_t count{0};
};

// Of `adapters` (DXGI's, their IDs in enumeration order), those with `wanted`.
AdapterIdMatch MatchAdapterByIds(const std::vector<PciIds>& adapters, PciIds wanted);

// The end of a Vulkan session's line when its adapter was looked for by PCI
// IDs: found (and among how many), or not, and `no_luid`, why the device's
// LUID could not be used.
std::string AdapterIdMatchText(const AdapterIdMatch& match, std::string_view no_luid);

// DXGI's user-mode driver version as a.b.c.d, and for NVIDIA (vendor 0x10de)
// first as NVIDIA shows it: 32.0.15.8142 is "581.42 (32.0.15.8142)".
std::string DxgiDriverVersionText(std::uint64_t version, std::optional<std::uint32_t> vendor_id);

// VkPhysicalDeviceProperties::driverVersion in hex, and for NVIDIA first as
// NVIDIA packs it (10 bits major, 8 bits minor): "581.42 (0x914a8000)".
std::string VulkanDriverVersionText(std::uint32_t version, std::optional<std::uint32_t> vendor_id);

// "512 MiB" below what rounds to 1024 MiB, "31.8 GiB" from there up (just
// under 1 GiB reads "1.0 GiB").
std::string VideoMemoryText(std::uint64_t bytes);

// The session log's line for one graphics API:
//   Session graphics: api=Direct3D 12, adapter="NVIDIA GeForce RTX 5090", vendor=0x10de,
//   driver=581.42 (32.0.15.8142), dedicatedVideoMemory=31.8 GiB, budget=29.6 GiB (process uses 1.2 GiB),
//   runtime="PimaxXR" 1.2.3
// Vulkan adds vulkanDriver= after driver=; anything not found is "unknown",
// and a problem ends the line after "; ". Digits never depend on the locale.
std::string FormatSessionGraphics(const SessionGraphics& graphics, std::string_view runtime_name,
                                  std::string_view runtime_version);

// What the session's graphics APIs run on (in depthxr_framegen, which is built
// with the Direct3D 12 and Vulkan types the layer lacks): the first Direct3D
// 11, Direct3D 12 or Vulkan binding in `create_info_next` (the application's
// XrSessionCreateInfo::next), then, when Frame Generation bridges the session,
// the Direct3D 12 binding `bridge_binding`, as `bridge_api`. Only the
// application's own device objects and DXGI factory queries are used; no
// device is made. A Vulkan device's adapter is DXGI's with its LUID, or, when
// the LUID cannot be had safely, DXGI's first with its PCI vendor and device
// ID (the line then says so). A session without one of the three bindings
// gets one entry that says so.
std::vector<SessionGraphics> QuerySessionGraphics(const void* create_info_next, const void* bridge_binding,
                                                  std::string_view bridge_api);

} // namespace depthxr
