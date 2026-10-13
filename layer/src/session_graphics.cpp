#include "depthxr/session_graphics.h"

#include <charconv>

namespace depthxr {
namespace {

constexpr std::uint32_t kNvidiaVendorId = 0x10de;

// `value` in lowercase hex, at least `digits` digits, with 0x.
std::string HexText(std::uint64_t value, int digits) {
    char buffer[17]{};
    const auto end = std::to_chars(buffer, buffer + sizeof(buffer) - 1, value, 16).ptr;
    std::string text(buffer, end);
    if (static_cast<int>(text.size()) < digits) {
        text.insert(0, static_cast<std::size_t>(digits) - text.size(), '0');
    }
    return "0x" + text;
}

// `hundredths` as a number with two decimals ("581.42", "551.06").
std::string HundredthsText(std::uint64_t hundredths) {
    const std::uint64_t fraction = hundredths % 100;
    return std::to_string(hundredths / 100) + (fraction < 10 ? ".0" : ".") + std::to_string(fraction);
}

bool IsNvidia(std::optional<std::uint32_t> vendor_id) {
    return vendor_id && *vendor_id == kNvidiaVendorId;
}

} // namespace

std::string DxgiDriverVersionText(std::uint64_t version, std::optional<std::uint32_t> vendor_id) {
    const std::uint64_t a = (version >> 48) & 0xffff;
    const std::uint64_t b = (version >> 32) & 0xffff;
    const std::uint64_t c = (version >> 16) & 0xffff;
    const std::uint64_t d = version & 0xffff;
    const std::string fields =
        std::to_string(a) + "." + std::to_string(b) + "." + std::to_string(c) + "." + std::to_string(d);
    if (!IsNvidia(vendor_id)) {
        return fields;
    }
    // NVIDIA's release number is the third field's last digit and the fourth
    // field, in hundredths: 32.0.15.8142 is 581.42.
    return HundredthsText((c % 10) * 10000 + d) + " (" + fields + ")";
}

std::string VulkanDriverVersionText(std::uint32_t version, std::optional<std::uint32_t> vendor_id) {
    const std::string raw = HexText(version, 8);
    if (!IsNvidia(vendor_id)) {
        return raw;
    }
    const std::uint32_t major = version >> 22;
    const std::uint32_t minor = (version >> 14) & 0xff;
    return std::to_string(major) + (minor < 10 ? ".0" : ".") + std::to_string(minor) + " (" + raw + ")";
}

std::string VideoMemoryText(std::uint64_t bytes) {
    constexpr std::uint64_t kMiB = 1ull << 20;
    constexpr std::uint64_t kGiB = 1ull << 30;
    // The unit follows the rounded figure: just under 1 GiB reads "1.0 GiB",
    // never "1024 MiB".
    const std::uint64_t mebibytes = bytes / kMiB + (bytes % kMiB >= kMiB / 2 ? 1 : 0);
    if (mebibytes < 1024) {
        return std::to_string(mebibytes) + " MiB";
    }
    // Tenths of a GiB, rounded.
    const std::uint64_t tenths = (bytes / kGiB) * 10 + ((bytes % kGiB) * 10 + kGiB / 2) / kGiB;
    return std::to_string(tenths / 10) + "." + std::to_string(tenths % 10) + " GiB";
}

AdapterIdMatch MatchAdapterByIds(const std::vector<PciIds>& adapters, PciIds wanted) {
    AdapterIdMatch match;
    for (std::size_t i = 0; i < adapters.size(); ++i) {
        if (adapters[i].vendor_id == wanted.vendor_id && adapters[i].device_id == wanted.device_id) {
            if (!match.first) {
                match.first = i;
            }
            ++match.count;
        }
    }
    return match;
}

std::string AdapterIdMatchText(const AdapterIdMatch& match, std::string_view no_luid) {
    if (!match.first) {
        return "DXGI has no adapter with the Vulkan device's PCI vendor and device ID, and its LUID is unknown: " +
               std::string(no_luid);
    }
    std::string text = "adapter matched by PCI vendor and device ID";
    if (match.count > 1) {
        text += " (the first of " + std::to_string(match.count) + " adapters with them)";
    }
    return text + ", not by LUID: " + std::string(no_luid);
}

std::string FormatSessionGraphics(const SessionGraphics& graphics, std::string_view runtime_name,
                                  std::string_view runtime_version) {
    const std::string unknown = "unknown";
    std::string line = "Session graphics: api=" + (graphics.api.empty() ? unknown : graphics.api);
    line += ", adapter=" + (graphics.adapter.empty() ? unknown : "\"" + graphics.adapter + "\"");
    line += ", vendor=" + (graphics.vendor_id ? HexText(*graphics.vendor_id, 4) : unknown);
    line += ", driver=" +
            (graphics.driver_version ? DxgiDriverVersionText(*graphics.driver_version, graphics.vendor_id) : unknown);
    if (graphics.vulkan_driver_version) {
        line += ", vulkanDriver=" + VulkanDriverVersionText(*graphics.vulkan_driver_version, graphics.vendor_id);
    }
    line += ", dedicatedVideoMemory=" +
            (graphics.dedicated_video_memory ? VideoMemoryText(*graphics.dedicated_video_memory) : unknown);
    line += ", budget=";
    if (graphics.budget) {
        line += VideoMemoryText(*graphics.budget);
        if (graphics.usage) {
            line += " (process uses " + VideoMemoryText(*graphics.usage) + ")";
        }
    } else {
        line += unknown;
    }
    line += ", runtime=";
    if (runtime_name.empty()) {
        line += unknown;
    } else {
        line += "\"" + std::string(runtime_name) + "\"";
        if (!runtime_version.empty()) {
            line += " " + std::string(runtime_version);
        }
    }
    if (!graphics.problem.empty()) {
        line += "; " + graphics.problem;
    }
    return line;
}

} // namespace depthxr
