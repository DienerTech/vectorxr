#pragma once

// FG_TEST_ADAPTER=<text>: the GPU tests and the probes run on the first
// hardware adapter whose name contains the text, ignoring case and the spaces
// around it (DXGI's adapter description, or Vulkan's deviceName), instead of
// the adapter they take by default; for example FG_TEST_ADAPTER=radeon for a
// processor's integrated AMD GPU beside an NVIDIA card. Set and naming no
// adapter a test can use, the test stops with a message that lists the
// adapters there are (suites that skip with 77 skip). Unset or empty, nothing
// changes. Only the tests' own devices follow it: a layer under test takes
// the adapter of the game's device, as it does in a session.

#include <windows.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace test_adapter {

inline constexpr const char* kVariable = "FG_TEST_ADAPTER";
inline constexpr const wchar_t* kWideVariable = L"FG_TEST_ADAPTER";

// An adapter as the tests choose among them.
struct Adapter {
    std::string name;
    // DXGI's software adapter or a Vulkan CPU implementation: listed, never chosen.
    bool software{false};
};

inline char Lower(char c) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

// Whether `name` contains `wanted`, ignoring ASCII case. An empty `wanted`
// matches every name.
inline bool NameMatches(std::string_view name, std::string_view wanted) {
    if (wanted.empty()) return true;
    return std::search(name.begin(), name.end(), wanted.begin(), wanted.end(),
                       [](char a, char b) { return Lower(a) == Lower(b); }) != name.end();
}

// `text` without the spaces, tabs and line ends around it.
inline std::string Trimmed(std::string_view text) {
    const auto space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!text.empty() && space(text.front())) text.remove_prefix(1);
    while (!text.empty() && space(text.back())) text.remove_suffix(1);
    return std::string(text);
}

// Microsoft's vendor ID: WARP, and the Basic Render Driver of a machine with no
// GPU.
inline constexpr UINT kMicrosoftVendorId = 0x1414;

// Whether DXGI's adapter renders on the processor. A hosted CI runner's Basic
// Render Driver comes without DXGI's software flag, so Microsoft's vendor ID
// counts too.
inline bool SoftwareDxgiAdapter(const DXGI_ADAPTER_DESC1& desc) {
    return (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0 || desc.VendorId == kMicrosoftVendorId;
}

// A DXGI adapter description (or any UTF-16 text) in UTF-8.
inline std::string Utf8(const wchar_t* text) {
    const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) return {};
    std::string utf8(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8.data(), size, nullptr, nullptr);
    utf8.resize(static_cast<std::size_t>(size) - 1);
    return utf8;
}

// FG_TEST_ADAPTER's text in UTF-8, as the adapters' names are; nullopt when
// it is unset, empty or only spaces. Read as UTF-16, so a name outside the
// ANSI code page arrives whole.
inline std::optional<std::string> Wanted() {
    const DWORD size = GetEnvironmentVariableW(kWideVariable, nullptr, 0);
    if (size <= 1) return std::nullopt;
    std::wstring value(size, L'\0');
    const DWORD length = GetEnvironmentVariableW(kWideVariable, value.data(), size);
    if (length == 0 || length >= size) return std::nullopt;
    value.resize(length);
    std::string wanted = Trimmed(Utf8(value.c_str()));
    if (wanted.empty()) return std::nullopt;
    return wanted;
}

// The index of the first of `adapters` that is not software and whose name
// contains `wanted`.
inline std::optional<std::size_t> FirstMatch(const std::vector<Adapter>& adapters, std::string_view wanted) {
    for (std::size_t i = 0; i < adapters.size(); ++i) {
        if (!adapters[i].software && NameMatches(adapters[i].name, wanted)) return i;
    }
    return std::nullopt;
}

// Why no adapter was chosen, with the adapters there are.
inline std::string NoMatch(std::string_view wanted, const std::vector<Adapter>& adapters) {
    std::string message = std::string(kVariable) + "=\"" + std::string(wanted) +
                          "\" names no hardware adapter this test can use; the adapters here: ";
    if (adapters.empty()) message += "none";
    for (std::size_t i = 0; i < adapters.size(); ++i) {
        message += (i ? ", \"" : "\"") + adapters[i].name + "\"" + (adapters[i].software ? " (software)" : "");
    }
    return message;
}

// Of the adapters `enumerate(index, &adapter)` lists in turn (an HRESULT,
// DXGI_ERROR_NOT_FOUND past the last), the first that is hardware and whose
// description contains `wanted`; nullptr, with `why`, when there is none.
template <typename Enumerate>
Microsoft::WRL::ComPtr<IDXGIAdapter1> FindDxgiAdapter(Enumerate&& enumerate, std::string_view wanted,
                                                      std::string& why) {
    std::vector<Microsoft::WRL::ComPtr<IDXGIAdapter1>> found;
    std::vector<Adapter> adapters;
    for (UINT index = 0;; ++index) {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        if (FAILED(enumerate(index, adapter.GetAddressOf()))) break;
        DXGI_ADAPTER_DESC1 desc{};
        if (FAILED(adapter->GetDesc1(&desc))) continue;
        found.push_back(adapter);
        adapters.push_back({Utf8(desc.Description), SoftwareDxgiAdapter(desc)});
    }
    if (const std::optional<std::size_t> chosen = FirstMatch(adapters, wanted)) return found[*chosen];
    why = NoMatch(wanted, adapters);
    return nullptr;
}

// The same in `factory`'s own order (EnumAdapters1).
inline Microsoft::WRL::ComPtr<IDXGIAdapter1> FindDxgiAdapter(IDXGIFactory1* factory, std::string_view wanted,
                                                             std::string& why) {
    const auto enumerate = [factory](UINT index, IDXGIAdapter1** adapter) {
        return factory->EnumAdapters1(index, adapter);
    };
    return FindDxgiAdapter(enumerate, wanted, why);
}

} // namespace test_adapter
