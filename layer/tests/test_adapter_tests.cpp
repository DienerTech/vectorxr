// FG_TEST_ADAPTER (test_adapter.h) without a GPU: name matching, the
// environment variable's parsing, the message when it names nothing, and the
// DXGI choice over stand-in adapters (no factory, no device).
#include <windows.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <wrl/implements.h>

#include "test_adapter.h"

#include <array>
#include <cstdlib>
#include <cwchar>
#include <iostream>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

void Expect(bool value, const std::string& message) {
    if (!value) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

// A DXGI adapter that only describes itself.
class StandInAdapter
    : public Microsoft::WRL::RuntimeClass<Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IDXGIAdapter1> {
public:
    StandInAdapter(const wchar_t* name, UINT flags) {
        wcsncpy_s(desc_.Description, name, _TRUNCATE);
        desc_.Flags = flags;
    }

    HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_ADAPTER_DESC1* desc) override {
        *desc = desc_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE EnumOutputs(UINT, IDXGIOutput**) override { return DXGI_ERROR_NOT_FOUND; }
    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_ADAPTER_DESC*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE CheckInterfaceSupport(REFGUID, LARGE_INTEGER*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT*, void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID, void**) override { return E_NOTIMPL; }

private:
    DXGI_ADAPTER_DESC1 desc_{};
};

// FG_TEST_ADAPTER as the test found it, put back at the end.
std::string g_saved;
bool g_was_set = false;

void SaveVariable() {
    const DWORD size = GetEnvironmentVariableA(test_adapter::kVariable, nullptr, 0);
    g_was_set = size > 0;
    if (g_was_set) {
        g_saved.assign(size, '\0');
        g_saved.resize(GetEnvironmentVariableA(test_adapter::kVariable, g_saved.data(), size));
    }
}

void RestoreVariable() {
    SetEnvironmentVariableA(test_adapter::kVariable, g_was_set ? g_saved.c_str() : nullptr);
}

void TestNameMatches() {
    using test_adapter::NameMatches;
    Expect(NameMatches("NVIDIA GeForce RTX 5090", "rtx 5090"), "a substring in another case matches");
    Expect(NameMatches("AMD Radeon(TM) Graphics", "RADEON") && NameMatches("AMD Radeon(TM) Graphics", "amd"),
           "the start and the middle of a name match");
    Expect(NameMatches("AMD Radeon(TM) Graphics", "Graphics"), "the end of a name matches");
    Expect(!NameMatches("AMD Radeon(TM) Graphics", "nvidia") && !NameMatches("AMD Radeon(TM) Graphics", "radeon pro"),
           "text the name lacks does not match");
    Expect(!NameMatches("AMD", "AMD Radeon"), "text longer than the name does not match");
    Expect(NameMatches("Intel(R) Arc(TM) Graphics", "") && !NameMatches("", "a") && NameMatches("", ""),
           "empty text matches any name; an empty name matches only empty text");
    // Bytes outside ASCII (UTF-8) are compared as they are.
    Expect(NameMatches("Radeon\xE2\x84\xA2 780M", "\xE2\x84\xA2 780m") && !NameMatches("Radeon\xE2\x84\xA2", "\xC3\xA9"),
           "UTF-8 bytes compare exactly");
}

void TestTrimmed() {
    using test_adapter::Trimmed;
    Expect(Trimmed("  radeon \t\r\n") == "radeon" && Trimmed("rtx 5090") == "rtx 5090" && Trimmed(" \t ").empty() &&
               Trimmed("").empty(),
           "the spaces around the text go, the ones inside stay");
}

void TestWanted() {
    using test_adapter::Wanted;
    SetEnvironmentVariableA(test_adapter::kVariable, nullptr);
    Expect(!Wanted(), "unset: no choice");
    SetEnvironmentVariableA(test_adapter::kVariable, "");
    Expect(!Wanted(), "empty: no choice");
    SetEnvironmentVariableA(test_adapter::kVariable, "   ");
    Expect(!Wanted(), "only spaces: no choice");
    SetEnvironmentVariableA(test_adapter::kVariable, " Radeon ");
    Expect(Wanted() == std::string("Radeon"), "the text without the spaces around it");
    SetEnvironmentVariableA(test_adapter::kVariable, "NVIDIA GeForce RTX 5090");
    Expect(Wanted() == std::string("NVIDIA GeForce RTX 5090"), "a whole name with spaces inside");
    const std::string long_name(300, 'x');
    SetEnvironmentVariableA(test_adapter::kVariable, long_name.c_str());
    Expect(Wanted() == long_name, "a long value is read whole");
    SetEnvironmentVariableA(test_adapter::kVariable, nullptr);
}

void TestFirstMatch() {
    using test_adapter::Adapter;
    using test_adapter::FirstMatch;
    const std::vector<Adapter> adapters{{"NVIDIA GeForce RTX 5090", false},
                                        {"AMD Radeon(TM) Graphics", false},
                                        {"Microsoft Basic Render Driver", true},
                                        {"AMD Radeon RX 7900 XTX", false}};
    Expect(FirstMatch(adapters, "radeon") == std::size_t{1}, "the first adapter that matches, in the list's order");
    Expect(FirstMatch(adapters, "7900") == std::size_t{3}, "a later adapter when only it matches");
    Expect(FirstMatch(adapters, "nvidia") == std::size_t{0}, "the first adapter");
    Expect(!FirstMatch(adapters, "basic render"), "a software adapter is never chosen");
    Expect(!FirstMatch(adapters, "intel") && !FirstMatch({}, "radeon"), "no match: no choice");
    Expect(FirstMatch(adapters, "") == std::size_t{0}, "empty text: the first hardware adapter");
}

void TestNoMatch() {
    using test_adapter::NoMatch;
    const std::string message =
        NoMatch("intel", {{"NVIDIA GeForce RTX 5090", false}, {"Microsoft Basic Render Driver", true}});
    Expect(message == "FG_TEST_ADAPTER=\"intel\" names no hardware adapter this test can use; the adapters here: "
                      "\"NVIDIA GeForce RTX 5090\", \"Microsoft Basic Render Driver\" (software)",
           "the message names the text and lists the adapters: " + message);
    Expect(NoMatch("radeon", {}) ==
               "FG_TEST_ADAPTER=\"radeon\" names no hardware adapter this test can use; the adapters here: none",
           "no adapters at all");
}

void TestUtf8() {
    Expect(test_adapter::Utf8(L"AMD Radeon(TM) Graphics") == "AMD Radeon(TM) Graphics", "ASCII descriptions");
    // U+2122 (trade mark sign), as some drivers name their GPUs.
    Expect(test_adapter::Utf8(L"Radeon\x2122 780M") == "Radeon\xE2\x84\xA2 780M", "a description outside ASCII");
    Expect(test_adapter::Utf8(L"").empty(), "an empty description");
}

void TestFindDxgiAdapter() {
    const std::array<ComPtr<IDXGIAdapter1>, 3> adapters{
        Microsoft::WRL::Make<StandInAdapter>(L"NVIDIA GeForce RTX 5090", 0u),
        Microsoft::WRL::Make<StandInAdapter>(L"AMD Radeon(TM) Graphics", 0u),
        Microsoft::WRL::Make<StandInAdapter>(L"Microsoft Basic Render Driver",
                                             static_cast<UINT>(DXGI_ADAPTER_FLAG_SOFTWARE))};
    int enumerated = 0;
    const auto enumerate = [&](UINT index, IDXGIAdapter1** adapter) -> HRESULT {
        ++enumerated;
        if (index >= adapters.size()) return DXGI_ERROR_NOT_FOUND;
        return adapters[index].CopyTo(adapter);
    };
    std::string why;
    ComPtr<IDXGIAdapter1> chosen = test_adapter::FindDxgiAdapter(enumerate, "radeon", why);
    Expect(chosen.Get() == adapters[1].Get() && why.empty(), "the AMD adapter for 'radeon'");
    Expect(enumerated == 4, "every adapter is enumerated, up to DXGI_ERROR_NOT_FOUND");
    chosen = test_adapter::FindDxgiAdapter(enumerate, "GEFORCE", why);
    Expect(chosen.Get() == adapters[0].Get(), "the NVIDIA adapter for 'GEFORCE'");
    chosen = test_adapter::FindDxgiAdapter(enumerate, "basic render", why);
    Expect(!chosen && why.find("\"Microsoft Basic Render Driver\" (software)") != std::string::npos &&
               why.find("\"AMD Radeon(TM) Graphics\"") != std::string::npos,
           "the software adapter is not chosen, and the message lists every adapter: " + why);
    why.clear();
    chosen = test_adapter::FindDxgiAdapter([](UINT, IDXGIAdapter1**) -> HRESULT { return DXGI_ERROR_NOT_FOUND; },
                                           "radeon", why);
    Expect(!chosen && why.find("the adapters here: none") != std::string::npos, "no adapters: " + why);
}

} // namespace

int main() {
    SaveVariable();
    TestNameMatches();
    TestTrimmed();
    TestWanted();
    TestFirstMatch();
    TestNoMatch();
    TestUtf8();
    TestFindDxgiAdapter();
    RestoreVariable();
    std::cout << "test adapter tests passed\n";
    return 0;
}
