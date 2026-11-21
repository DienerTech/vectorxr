// The submitted-image module and screenshots (submitted_image.h,
// screenshot.h). No argument: the CPU half (formats, pixel conversion, view
// resolution, names and folders, the PNG round trip, the settings, the
// binding's press, and a whole press against a stand-in GPU). `d3d11` and
// `d3d12`: readback on the software adapter (WARP), never a hardware one.

#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <openxr/openxr_platform.h>

#include "depthxr/config_parser.h"
#include "depthxr/screenshot.h"
#include "depthxr/settings_resolver.h"
#include "depthxr/submitted_image.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <stdexcept>
#include <set>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

using namespace depthxr;
using Microsoft::WRL::ComPtr;

namespace {

int checks = 0;

void Expect(bool ok, const std::string& why) {
    ++checks;
    if (!ok) {
        std::cerr << "FAILED: " << why << '\n';
        std::exit(1);
    }
}

XrSwapchain Handle(std::uintptr_t value) { return reinterpret_cast<XrSwapchain>(value); }

std::filesystem::path ScratchFolder(const wchar_t* name) {
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    std::filesystem::path folder = std::filesystem::path(temp) /
                                   (std::wstring(L"vectorxr-imaging-tests-") + std::to_wstring(GetCurrentProcessId())) / name;
    std::error_code ignored;
    std::filesystem::remove_all(folder, ignored);
    std::filesystem::create_directories(folder, ignored);
    return folder;
}

void RemoveScratch() {
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    std::error_code ignored;
    std::filesystem::remove_all(std::filesystem::path(temp) /
                                    (std::wstring(L"vectorxr-imaging-tests-") + std::to_wstring(GetCurrentProcessId())),
                                ignored);
}

// ---- CPU ----

void TestFormats() {
    using Api = SubmittedImageApi;
    using Kind = SubmittedPixelKind;
    const auto rgba_srgb = ClassifyFormat(Api::kD3D11, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
    Expect(rgba_srgb.kind == Kind::kUnorm8 && rgba_srgb.srgb && !rgba_srgb.bgra && !rgba_srgb.typeless &&
               rgba_srgb.view_format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB && rgba_srgb.bytes_per_pixel == 4,
           "R8G8B8A8_UNORM_SRGB is 8-bit sRGB, red first");
    const auto bgra = ClassifyFormat(Api::kD3D12, DXGI_FORMAT_B8G8R8A8_UNORM);
    Expect(bgra.kind == Kind::kUnorm8 && !bgra.srgb && bgra.bgra, "B8G8R8A8_UNORM is 8-bit, blue first");
    Expect(ClassifyFormat(Api::kD3D11, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB).srgb &&
               ClassifyFormat(Api::kD3D11, DXGI_FORMAT_B8G8R8X8_UNORM).bgra,
           "The other blue-first formats classify");
    const auto typeless = ClassifyFormat(Api::kD3D11, DXGI_FORMAT_R8G8B8A8_TYPELESS);
    Expect(typeless.typeless && !typeless.srgb && typeless.view_format == DXGI_FORMAT_R8G8B8A8_UNORM,
           "A typeless family is viewed as UNORM without the hint");
    const auto typeless_srgb = ClassifyFormat(Api::kD3D11, DXGI_FORMAT_B8G8R8A8_TYPELESS, true);
    Expect(typeless_srgb.typeless && typeless_srgb.srgb && typeless_srgb.bgra &&
               typeless_srgb.view_format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
           "A typeless family is viewed as sRGB with the hint");
    const auto ten = ClassifyFormat(Api::kD3D12, DXGI_FORMAT_R10G10B10A2_UNORM);
    Expect(ten.kind == Kind::kUnorm10 && ten.bytes_per_pixel == 4 && !ten.srgb, "R10G10B10A2_UNORM is 10-bit");
    const auto half = ClassifyFormat(Api::kD3D12, DXGI_FORMAT_R16G16B16A16_FLOAT);
    Expect(half.kind == Kind::kFloat16 && half.bytes_per_pixel == 8, "R16G16B16A16_FLOAT is half float");
    Expect(ClassifyFormat(Api::kD3D11, DXGI_FORMAT_R16G16B16A16_TYPELESS).view_format == DXGI_FORMAT_R16G16B16A16_FLOAT &&
               ClassifyFormat(Api::kD3D11, DXGI_FORMAT_R10G10B10A2_TYPELESS).view_format == DXGI_FORMAT_R10G10B10A2_UNORM,
           "The wide typeless families have one view");
    Expect(ClassifyFormat(Api::kD3D11, DXGI_FORMAT_D32_FLOAT).kind == Kind::kUnknown &&
               ClassifyFormat(Api::kD3D11, DXGI_FORMAT_R11G11B10_FLOAT).kind == Kind::kUnknown &&
               ClassifyFormat(Api::kNone, DXGI_FORMAT_R8G8B8A8_UNORM).kind == Kind::kUnknown,
           "Depth, unread color formats and no API are unknown");
    // VkFormat numbers: R8G8B8A8_SRGB 43, B8G8R8A8_UNORM 44, A2B10G10R10 64, R16G16B16A16_SFLOAT 97.
    Expect(ClassifyFormat(Api::kVulkan, 43).srgb && ClassifyFormat(Api::kVulkan, 44).bgra &&
               ClassifyFormat(Api::kVulkan, 64).kind == Kind::kUnorm10 &&
               ClassifyFormat(Api::kVulkan, 97).kind == Kind::kFloat16 &&
               ClassifyFormat(Api::kVulkan, 28).kind == Kind::kUnknown,
           "Vulkan formats classify by their own numbers");
    Expect(SameDxgiFamily(DXGI_FORMAT_R8G8B8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) &&
               SameDxgiFamily(DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB) &&
               SameDxgiFamily(DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_D32_FLOAT) &&
               !SameDxgiFamily(DXGI_FORMAT_B8G8R8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_UNORM) &&
               !SameDxgiFamily(DXGI_FORMAT_R16G16B16A16_TYPELESS, DXGI_FORMAT_R10G10B10A2_UNORM),
           "Format families match within, never across");
}

std::uint16_t Half(float value) {
    // Exact for the values the tests use (powers of two and sums of a few).
    if (value == 0.0f) return 0;
    const std::uint16_t sign = value < 0 ? 0x8000 : 0;
    int exponent = 0;
    const float mantissa = std::frexp(std::fabs(value), &exponent); // [0.5, 1)
    return static_cast<std::uint16_t>(sign | ((exponent + 14) << 10) |
                                      (static_cast<int>(std::lround((mantissa * 2.0f - 1.0f) * 1024.0f)) & 0x3ff));
}

void TestPixelConversion() {
    Expect(HalfToFloat(0x3C00) == 1.0f && HalfToFloat(0x3800) == 0.5f && HalfToFloat(0xC000) == -2.0f &&
               HalfToFloat(0x0000) == 0.0f && HalfToFloat(0x0001) == std::ldexp(1.0f, -24) &&
               std::isinf(HalfToFloat(0x7C00)) && std::isnan(HalfToFloat(0x7E00)),
           "Half floats decode, denormals and specials included");
    Expect(Half(1.0f) == 0x3C00 && Half(0.5f) == 0x3800 && Half(-2.0f) == 0xC000, "The test's half encoder agrees");
    Expect(LinearToSrgb8(0.0f) == 0 && LinearToSrgb8(1.0f) == 255 && LinearToSrgb8(0.5f) == 188 &&
               LinearToSrgb8(0.25f) == 137 && LinearToSrgb8(0.001f) == 3 && LinearToSrgb8(-1.0f) == 0 &&
               LinearToSrgb8(7.0f) == 255 && LinearToSrgb8(std::numeric_limits<float>::quiet_NaN()) == 0 &&
               LinearToSrgb8(std::numeric_limits<float>::infinity()) == 255,
           "Linear values encode to sRGB, clamped");

    // 8-bit, red first, with a row pitch wider than the row.
    {
        const std::uint8_t in[2 * 12] = {1, 2, 3, 4, 5, 6, 7, 0, 99, 99, 99, 99, 9, 10, 11, 12, 13, 14, 15, 200, 99, 99, 99, 99};
        SubmittedPixels pixels{in, 12, 2, 2, ClassifyFormat(SubmittedImageApi::kD3D11, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)};
        std::uint8_t out[16]{};
        Expect(ConvertToRgba8(pixels, out), "8-bit pixels convert");
        const std::uint8_t want[16] = {1, 2, 3, 255, 5, 6, 7, 255, 9, 10, 11, 255, 13, 14, 15, 255};
        Expect(std::memcmp(out, want, 16) == 0, "8-bit pixels are kept as they are, alpha 255, the pitch's padding skipped");
        pixels.format = ClassifyFormat(SubmittedImageApi::kD3D11, DXGI_FORMAT_B8G8R8A8_UNORM);
        Expect(ConvertToRgba8(pixels, out), "Blue-first pixels convert");
        const std::uint8_t swapped[16] = {3, 2, 1, 255, 7, 6, 5, 255, 11, 10, 9, 255, 15, 14, 13, 255};
        Expect(std::memcmp(out, swapped, 16) == 0, "Blue-first pixels have red and blue swapped");
    }
    // 10-bit: linear to sRGB.
    {
        const std::uint32_t in[2] = {1023u | (0u << 10) | (512u << 20) | (3u << 30), 0u};
        SubmittedPixels pixels{reinterpret_cast<const std::uint8_t*>(in), 8, 2, 1,
                               ClassifyFormat(SubmittedImageApi::kD3D12, DXGI_FORMAT_R10G10B10A2_UNORM)};
        std::uint8_t out[8]{};
        Expect(ConvertToRgba8(pixels, out), "10-bit pixels convert");
        Expect(out[0] == 255 && out[1] == 0 && out[2] == LinearToSrgb8(512.0f / 1023.0f) && out[3] == 255 &&
                   out[4] == 0 && out[7] == 255,
               "10-bit pixels are encoded from linear to sRGB");
    }
    // Half float: clamped, linear to sRGB.
    {
        const std::uint16_t in[8] = {Half(0.5f), Half(4.0f), Half(-1.0f), Half(0.25f), 0x7E00, Half(1.0f), 0, 0};
        SubmittedPixels pixels{reinterpret_cast<const std::uint8_t*>(in), 16, 2, 1,
                               ClassifyFormat(SubmittedImageApi::kD3D12, DXGI_FORMAT_R16G16B16A16_FLOAT)};
        std::uint8_t out[8]{};
        Expect(ConvertToRgba8(pixels, out), "Half-float pixels convert");
        Expect(out[0] == 188 && out[1] == 255 && out[2] == 0 && out[3] == 255 && out[4] == 0 && out[5] == 255 &&
                   out[6] == 0 && out[7] == 255,
               "Half-float pixels are clamped to 0..1 and encoded to sRGB, alpha 255 whatever theirs");
        pixels.row_pitch = 15;
        Expect(!ConvertToRgba8(pixels, out), "A row pitch shorter than the row is refused");
        pixels.row_pitch = 16;
        pixels.format = ClassifyFormat(SubmittedImageApi::kD3D12, DXGI_FORMAT_R11G11B10_FLOAT);
        Expect(!ConvertToRgba8(pixels, out), "An unknown kind is refused");
    }
}

void TestNames() {
    Expect(ScreenshotExeStem("DCS.exe") == "DCS" && ScreenshotExeStem("FlightSimulator2024.exe") == "FlightSimulator2024" &&
               ScreenshotExeStem("C:\\Games\\a.b\\My Game.exe") == "My Game" && ScreenshotExeStem("a:b?c*.exe") == "a_b_c_" &&
               ScreenshotExeStem("") == "game" && ScreenshotExeStem(".exe") == ".exe" && ScreenshotExeStem("noext") == "noext" &&
               ScreenshotExeStem(" . .exe") == "game",
           "The game's stem drops the folder and extension and what a file name cannot hold");
    const ScreenshotTime time{2026, 11, 10, 7, 5, 9, 42};
    Expect(ScreenshotFileName("DCS.exe", time, 'L') == "DCS_20261110_070509_042_L.png" &&
               ScreenshotFileName("DCS.exe", time, 'R') == "DCS_20261110_070509_042_R.png",
           "The file is <exe>_<yyyyMMdd_HHmmss_mmm>_<eye>.png");
    const std::filesystem::path pictures = L"C:\\Users\\me\\Pictures";
    const std::filesystem::path local = L"C:\\Users\\me\\AppData\\Local";
    Expect(ScreenshotFolder("", "DCS.exe", pictures, local) == pictures / L"VectorXR" / L"DCS",
           "The default folder is VectorXR\\<exe> under Pictures");
    Expect(ScreenshotFolder("D:\\Shots", "DCS.exe", pictures, local) == std::filesystem::path(L"D:\\Shots\\DCS"),
           "A custom folder gets the game's folder inside");
    Expect(ScreenshotFolder("Shots", "DCS.exe", pictures, local) == pictures / L"VectorXR" / L"DCS",
           "A custom folder that is not absolute is not used");
    Expect(ScreenshotFolder("", "DCS.exe", {}, local) == local / L"VectorXR" / L"screenshots" / L"DCS",
           "Without a Pictures folder screenshots go under the local application data");
    // UTF-8 in, UTF-16 out.
    Expect(ScreenshotFolder("D:\\Bilder \xC3\xA4", "DCS.exe", pictures, local) ==
               std::filesystem::path(L"D:\\Bilder \u00E4\\DCS"),
           "A custom folder is read as UTF-8");
    Expect(!PicturesFolder().empty(), "This machine has a Pictures folder");
}

void TestEyes() {
    const auto letters = [](const std::vector<ScreenshotEye>& eyes) {
        std::string text;
        for (const auto& eye : eyes) text += std::to_string(eye.view_index) + eye.letter;
        return text;
    };
    Expect(letters(SelectScreenshotEyes(2, ScreenshotEyes::Both)) == "0L1R" &&
               letters(SelectScreenshotEyes(2, ScreenshotEyes::Left)) == "0L" &&
               letters(SelectScreenshotEyes(2, ScreenshotEyes::Right)) == "1R",
           "Stereo: left, right or both");
    Expect(letters(SelectScreenshotEyes(4, ScreenshotEyes::Both)) == "0L1R" &&
               letters(SelectScreenshotEyes(4, ScreenshotEyes::Right)) == "1R",
           "Quad views not composed: the stereo views");
    Expect(letters(SelectScreenshotEyes(1, ScreenshotEyes::Right)) == "0L" && SelectScreenshotEyes(0, ScreenshotEyes::Both).empty(),
           "One view is saved as L; none, nothing");
}

void TestBindingPress() {
    ScreenshotBindingEdge edge;
    Expect(!edge.Update(true), "A binding held at the first poll is no press");
    Expect(!edge.Update(true) && !edge.Update(false), "Held, then released: no press");
    Expect(edge.Update(true), "Down after up is a press");
    Expect(!edge.Update(true), "A held binding presses once");
    Expect(!edge.Update(false) && edge.Update(true), "Each new press counts");
    edge.Reset();
    Expect(!edge.Update(true) && !edge.Update(false) && edge.Update(true), "After a reset the first poll primes again");
    ScreenshotBindingEdge fresh;
    Expect(!fresh.Update(false) && fresh.Update(true), "Up at the first poll, then down: a press");
}

void TestPngRoundTrip() {
    const std::filesystem::path folder = ScratchFolder(L"png");
    const std::uint32_t width = 37, height = 21;
    std::vector<std::uint8_t> rgba(width * height * 4);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            std::uint8_t* p = &rgba[(y * width + x) * 4];
            p[0] = static_cast<std::uint8_t>(x * 7);
            p[1] = static_cast<std::uint8_t>(y * 11);
            p[2] = static_cast<std::uint8_t>(x + y * 3);
            p[3] = static_cast<std::uint8_t>(x); // ignored: the file is opaque
        }
    }
    // The folder is created with the file, non-ASCII included.
    const std::filesystem::path path = folder / L"sp\u00E4t" / L"shot_L.png";
    std::string error;
    Expect(WritePng(path, rgba.data(), width, height, error), "A PNG is written: " + error);
    std::vector<std::uint8_t> read;
    std::uint32_t read_width = 0, read_height = 0;
    Expect(ReadPng(path, read, read_width, read_height, error), "The PNG is read back: " + error);
    Expect(read_width == width && read_height == height && read.size() == rgba.size(), "The PNG has the image's size");
    bool same = true;
    for (std::size_t i = 0; i < rgba.size(); i += 4) {
        same = same && read[i] == rgba[i] && read[i + 1] == rgba[i + 1] && read[i + 2] == rgba[i + 2] && read[i + 3] == 255;
    }
    Expect(same, "The PNG holds the same colors, opaque");
    std::size_t files = 0;
    for (const auto& entry : std::filesystem::directory_iterator(path.parent_path())) {
        (void)entry;
        ++files;
    }
    Expect(files == 1, "No temporary file is left beside the PNG");
    // Written again over itself (a second press in the same millisecond).
    rgba[0] = 200;
    Expect(WritePng(path, rgba.data(), width, height, error) && ReadPng(path, read, read_width, read_height, error) &&
               read[0] == 200,
           "A PNG replaces the file of the same name");
    // A taller image than one band of rows.
    const std::uint32_t tall_width = 2048, tall_height = 1200;
    std::vector<std::uint8_t> tall(static_cast<std::size_t>(tall_width) * tall_height * 4);
    for (std::size_t i = 0; i < tall.size(); ++i) tall[i] = static_cast<std::uint8_t>((i * 2654435761u) >> 24);
    Expect(WritePng(folder / L"tall.png", tall.data(), tall_width, tall_height, error) &&
               ReadPng(folder / L"tall.png", read, read_width, read_height, error) && read_height == tall_height,
           "A large PNG is written in bands: " + error);
    same = true;
    for (std::size_t i = 0; i < tall.size(); i += 4) {
        same = same && read[i] == tall[i] && read[i + 1] == tall[i + 1] && read[i + 2] == tall[i + 2];
    }
    Expect(same, "The large PNG holds the same colors");
    Expect(!WritePng(folder / L"none.png", nullptr, 4, 4, error) && !WritePng(folder / L"none.png", rgba.data(), 0, 4, error) &&
               !std::filesystem::exists(folder / L"none.png"),
           "Nothing is written for no pixels");
    Expect(!ReadPng(folder / L"missing.png", read, read_width, read_height, error), "A missing PNG is not read");
}

// A PNG's size is read before it is decoded: one too large is refused, and a
// large one is shrunk as it is decoded where the caller asks.
void TestPngLimits() {
    const std::filesystem::path folder = ScratchFolder(L"png-limits");
    std::string error;
    std::vector<std::uint8_t> read;
    std::uint32_t read_width = 0, read_height = 0;
    // A real picture, wider than the caller wants: left half opaque red,
    // right half fully transparent green.
    const std::uint32_t width = 3000, height = 120;
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(width) * height * 4);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            std::uint8_t* p = &rgba[(static_cast<std::size_t>(y) * width + x) * 4];
            p[0] = 250;
            p[1] = 10;
            p[2] = 20;
            p[3] = 255;
        }
    }
    Expect(WritePng(folder / L"wide.png", rgba.data(), width, height, error), "A wide PNG is written: " + error);
    Expect(ReadPng(folder / L"wide.png", read, read_width, read_height, error, 256) && read_width == 256 &&
               read_height == 10 && read.size() == 256u * 10u * 4u,
           "A PNG larger than asked for is shrunk to fit as it is decoded: " + error + " " +
               std::to_string(read_width) + "x" + std::to_string(read_height));
    const std::uint8_t* middle = &read[(5u * 256u + 128u) * 4];
    Expect(std::abs(middle[0] - 250) <= 2 && std::abs(middle[1] - 10) <= 2 && std::abs(middle[2] - 20) <= 2 &&
               middle[3] == 255,
           "The shrunk PNG keeps its colors, red first, alpha straight");
    Expect(ReadPng(folder / L"wide.png", read, read_width, read_height, error, 4096) && read_width == width &&
               read_height == height,
           "A PNG no larger than asked for is read as it is");
    // Headers alone, of sizes nothing should decode: refused by their size.
    const auto crc32 = [](const std::vector<std::uint8_t>& bytes) {
        std::uint32_t crc = 0xFFFFFFFFu;
        for (const std::uint8_t byte : bytes) {
            crc ^= byte;
            for (int bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
        return ~crc;
    };
    const auto big_endian = [](std::vector<std::uint8_t>& out, std::uint32_t value) {
        for (int shift = 24; shift >= 0; shift -= 8) out.push_back(static_cast<std::uint8_t>(value >> shift));
    };
    const auto header_only = [&](const wchar_t* name, std::uint32_t w, std::uint32_t h) {
        std::vector<std::uint8_t> file = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
        std::vector<std::uint8_t> chunk = {'I', 'H', 'D', 'R'};
        big_endian(chunk, w);
        big_endian(chunk, h);
        chunk.insert(chunk.end(), {8, 6, 0, 0, 0});
        big_endian(file, 13);
        file.insert(file.end(), chunk.begin(), chunk.end());
        big_endian(file, crc32(chunk));
        // One (empty) image data chunk and the end.
        std::vector<std::uint8_t> data = {'I', 'D', 'A', 'T', 0x78, 0x9C, 0x03, 0x00, 0x00, 0x00, 0x00, 0x01};
        big_endian(file, static_cast<std::uint32_t>(data.size() - 4));
        file.insert(file.end(), data.begin(), data.end());
        big_endian(file, crc32(data));
        std::vector<std::uint8_t> end = {'I', 'E', 'N', 'D'};
        big_endian(file, 0);
        file.insert(file.end(), end.begin(), end.end());
        big_endian(file, crc32(end));
        const std::filesystem::path path = folder / name;
        FILE* out = nullptr;
        _wfopen_s(&out, path.c_str(), L"wb");
        Expect(out != nullptr, "A header-only PNG is made");
        fwrite(file.data(), 1, file.size(), out);
        fclose(out);
        return path;
    };
    error.clear();
    read.clear();
    Expect(!ReadPng(header_only(L"long.png", 20000, 16), read, read_width, read_height, error) && read.empty() &&
               error.find("larger than") != std::string::npos,
           "A PNG longer than 16384 a side is refused by its header, nothing allocated: " + error);
    error.clear();
    Expect(!ReadPng(header_only(L"huge.png", 12000, 12000), read, read_width, read_height, error, 2048) &&
               read.empty() && error.find("larger than") != std::string::npos,
           "A PNG of more than 8192x8192 pixels is refused by its header, even to be shrunk: " + error);
}

void TestViewResolution() {
    std::map<XrSwapchain, SubmittedSwapchainState> swapchains;
    SubmittedSwapchainState left;
    left.width = 4000;
    left.height = 3000;
    left.format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    left.image_index = 2;
    left.has_image = true;
    left.usage = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    swapchains[Handle(1)] = left;
    SubmittedSwapchainState both = left; // one double-wide array swapchain
    both.width = 8000;
    both.array_size = 2;
    both.sample_count = 4;
    both.image_index = 1;
    swapchains[Handle(2)] = both;
    SubmittedSwapchainState unreleased = left;
    unreleased.has_image = false;
    swapchains[Handle(3)] = unreleased;
    const SubmittedSwapchainLookup lookup = [&](XrSwapchain swapchain, SubmittedSwapchainState& state) {
        const auto found = swapchains.find(swapchain);
        if (found == swapchains.end()) return false;
        state = found->second;
        return true;
    };
    const auto view = [](XrSwapchain swapchain, int x, int y, int width, int height, std::uint32_t slice) {
        XrCompositionLayerProjectionView out{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
        out.subImage.swapchain = swapchain;
        out.subImage.imageRect = {{x, y}, {width, height}};
        out.subImage.imageArrayIndex = slice;
        return out;
    };
    std::vector<SubmittedView> views;
    std::string problem;

    XrFrameEndInfo frame{XR_TYPE_FRAME_END_INFO};
    Expect(!ResolveSubmittedViews(frame, lookup, views, problem) && !problem.empty() && views.empty(),
           "A frame without layers has no views");
    XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
    XrCompositionLayerProjection empty{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    XrCompositionLayerProjectionView stereo[2] = {view(Handle(1), 0, 0, 4000, 3000, 0), view(Handle(2), 4000, 100, 4000, 2900, 1)};
    XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    projection.viewCount = 2;
    projection.views = stereo;
    const XrCompositionLayerBaseHeader* layers[] = {nullptr, reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad),
                                                    reinterpret_cast<const XrCompositionLayerBaseHeader*>(&empty),
                                                    reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection)};
    frame.layerCount = 3;
    frame.layers = layers;
    Expect(!ResolveSubmittedViews(frame, lookup, views, problem), "Quads and a projection without views are not views");
    frame.layerCount = 4;
    Expect(ResolveSubmittedViews(frame, lookup, views, problem) && views.size() == 2, "The first projection with views is resolved");
    Expect(views[0].resolved && views[0].swapchain == Handle(1) && views[0].view_index == 0 && views[0].image_index == 2 &&
               views[0].array_slice == 0 && views[0].rect.extent.width == 4000 && views[0].width == 4000 &&
               views[0].height == 3000 && views[0].format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB &&
               views[0].array_size == 1 && views[0].sample_count == 1 &&
               views[0].usage == XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT,
           "A view carries its swapchain's released image, size, format and usage");
    Expect(views[1].resolved && views[1].image_index == 1 && views[1].array_slice == 1 && views[1].rect.offset.x == 4000 &&
               views[1].rect.offset.y == 100 && views[1].rect.extent.height == 2900 && views[1].width == 8000 &&
               views[1].array_size == 2 && views[1].sample_count == 4 && views[1].view_index == 1,
           "A view's rectangle and array slice are its own");

    // One bad view does not unresolve the others.
    stereo[1] = view(Handle(2), 4000, 100, 4001, 2900, 1);
    Expect(ResolveSubmittedViews(frame, lookup, views, problem) && views[0].resolved && !views[1].resolved &&
               views[1].problem.find("outside") != std::string::npos,
           "A rectangle outside the image leaves its view unresolved");
    stereo[1] = view(Handle(2), 0, 0, 0, 2900, 1);
    Expect(ResolveSubmittedViews(frame, lookup, views, problem) && !views[1].resolved, "An empty rectangle is unresolved");
    stereo[1] = view(Handle(2), -1, 0, 100, 100, 1);
    Expect(ResolveSubmittedViews(frame, lookup, views, problem) && !views[1].resolved, "A negative offset is unresolved");
    stereo[1] = view(Handle(2), 0, 0, std::numeric_limits<int>::max(), 100, 1);
    stereo[1].subImage.imageRect.offset.x = std::numeric_limits<int>::max();
    Expect(ResolveSubmittedViews(frame, lookup, views, problem) && !views[1].resolved, "A rectangle that overflows is unresolved");
    stereo[1] = view(Handle(2), 0, 0, 100, 100, 2);
    Expect(ResolveSubmittedViews(frame, lookup, views, problem) && !views[1].resolved &&
               views[1].problem.find("slice") != std::string::npos,
           "An array slice outside the array is unresolved");
    stereo[1] = view(Handle(9), 0, 0, 100, 100, 0);
    Expect(ResolveSubmittedViews(frame, lookup, views, problem) && !views[1].resolved &&
               views[1].problem.find("does not know") != std::string::npos,
           "An unknown swapchain is unresolved");
    stereo[1] = view(XR_NULL_HANDLE, 0, 0, 100, 100, 0);
    Expect(ResolveSubmittedViews(frame, lookup, views, problem) && !views[1].resolved, "A null swapchain is unresolved");
    stereo[1] = view(Handle(3), 0, 0, 100, 100, 0);
    Expect(ResolveSubmittedViews(frame, lookup, views, problem) && !views[1].resolved &&
               views[1].problem.find("released") != std::string::npos,
           "A swapchain that released no image is unresolved");
    Expect(ResolveSubmittedViews(frame, nullptr, views, problem) && !views[0].resolved, "Without a lookup nothing resolves");

    // Quad views not composed by the layer: four views, the stereo pair first.
    XrCompositionLayerProjectionView four[4] = {view(Handle(1), 0, 0, 2000, 1500, 0), view(Handle(1), 2000, 0, 2000, 1500, 0),
                                                view(Handle(9), 0, 0, 800, 800, 0), view(Handle(9), 800, 0, 800, 800, 0)};
    projection.viewCount = 4;
    projection.views = four;
    Expect(ResolveSubmittedViews(frame, lookup, views, problem) && views.size() == 4 && views[0].resolved &&
               views[1].resolved && views[1].rect.offset.x == 2000 && !views[2].resolved && !views[3].resolved,
           "A four-view layer resolves view by view");
    const auto eyes = SelectScreenshotEyes(static_cast<std::uint32_t>(views.size()), ScreenshotEyes::Both);
    Expect(eyes.size() == 2 && views[eyes[0].view_index].resolved && views[eyes[1].view_index].resolved,
           "Its stereo views are the ones saved");
}

void TestSettings() {
    const auto config = [](const std::string& core_extra) {
        return R"({"version":3,"core":{"enabled":true,"logLevel":"info")" + core_extra +
               R"(},"applications":[],"modules":{"depthxr":{"enabled":false,"defaults":{"stereoBoost":1,"convergence":0},)"
               R"("bindings":{"toggleEnabled":{"type":"none"}},"profiles":[]},)"
               R"("pivotxr":{"enabled":false,"defaults":{},"profiles":[]}}})";
    };
    const ParseResult absent = ParseConfig(config(""));
    Expect(absent.ok, "A config without core.screenshot parses: " + absent.error);
    Expect(absent.document.core.screenshot.bindings.empty() && absent.document.core.screenshot.eyes == ScreenshotEyes::Both &&
               absent.document.core.screenshot.folder.empty(),
           "Without core.screenshot nothing is bound, both eyes, the default folder");
    const ParseResult full = ParseConfig(config(
        R"(,"screenshot":{"bindings":[{"type":"keyboard","chord":["Ctrl","Alt","F12"],"sound":{"enabled":true}},)"
        R"({"type":"none"},{"type":"device","deviceGuid":"{1}","inputPath":"button-7"}],"eyes":"right","folder":"D:\\Shots"})"));
    Expect(full.ok && full.warnings.empty(), "core.screenshot parses without warnings: " + full.error);
    const ScreenshotSettings& screenshot = full.document.core.screenshot;
    Expect(screenshot.bindings.size() == 2 && screenshot.bindings[0].type == InputBindingType::Keyboard &&
               screenshot.bindings[0].chord.size() == 3 &&
               screenshot.bindings[0].sound.enabled && screenshot.bindings[1].type == InputBindingType::Device &&
               screenshot.bindings[1].input_path == "button-7",
           "The screenshot bindings are an ordinary list, a none entry dropped");
    Expect(screenshot.eyes == ScreenshotEyes::Right && screenshot.folder == "D:\\Shots", "Eyes and folder are read");
    const ResolvedRuntimeConfig resolved = ResolveRuntimeConfig(full.document, "DCS.exe");
    Expect(resolved.core.screenshot.bindings.size() == 2 && resolved.core.screenshot.eyes == ScreenshotEyes::Right &&
               resolved.core.screenshot.folder == "D:\\Shots",
           "Screenshots are global: every application resolves to them");
    Expect(ParseConfig(config(R"(,"screenshot":{"eyes":"left"})")).document.core.screenshot.eyes == ScreenshotEyes::Left &&
               ParseConfig(config(R"(,"screenshot":{"eyes":"both"})")).document.core.screenshot.eyes == ScreenshotEyes::Both &&
               ParseConfig(config(R"(,"screenshot":{})")).ok,
           "Each eyes value parses; an empty block is the defaults");
    Expect(!ParseConfig(config(R"(,"screenshot":{"eyes":"up"})")).ok && !ParseConfig(config(R"(,"screenshot":{"eyes":3})")).ok &&
               !ParseConfig(config(R"(,"screenshot":{"folder":7})")).ok && !ParseConfig(config(R"(,"screenshot":[])")).ok &&
               !ParseConfig(config(R"(,"screenshot":{"bindings":{}})")).ok,
           "Invalid screenshot settings are rejected");
    const ParseResult unknown = ParseConfig(config(R"(,"screenshot":{"format":"jpg"})"));
    Expect(unknown.ok && unknown.warnings.size() == 1 && unknown.warnings[0].find("format") != std::string::npos,
           "An unknown screenshot key is ignored with a warning");
}

// A stand-in for the GPU half: images are colors, readbacks become ready
// after a number of polls.
class FakeGpu : public SubmittedImageGpu {
  public:
    struct Ticket {
        std::vector<std::uint8_t> pixels;
        SubmittedView view;
        int polls_left{0};
        bool finished{false};
        bool fails{false};
    };
    std::map<std::uint64_t, Ticket> tickets;
    std::uint64_t next{1};
    int pending_polls{2};
    int fail_readback_of_view{-1};
    int fail_poll_of_view{-1};
    bool reset{false};

    const char* Name() const override { return "Fake"; }
    SubmittedImageApi Api() const override { return SubmittedImageApi::kD3D11; }
    bool Track(XrSwapchain, const XrSwapchainImageBaseHeader*, std::uint32_t, std::string&) override { return true; }
    void Forget(XrSwapchain) override {}
    bool Tracked(XrSwapchain) const override { return true; }
    std::uint64_t Readback(const SubmittedView& view, std::string& error) override {
        if (static_cast<int>(view.view_index) == fail_readback_of_view) {
            error = "no memory";
            return 0;
        }
        Ticket ticket;
        ticket.view = view;
        ticket.polls_left = pending_polls;
        ticket.fails = static_cast<int>(view.view_index) == fail_poll_of_view;
        const std::uint32_t width = view.rect.extent.width, height = view.rect.extent.height;
        ticket.pixels.resize(static_cast<std::size_t>(width) * height * 4);
        for (std::uint32_t y = 0; y < height; ++y) {
            for (std::uint32_t x = 0; x < width; ++x) {
                std::uint8_t* p = &ticket.pixels[(static_cast<std::size_t>(y) * width + x) * 4];
                p[0] = static_cast<std::uint8_t>(x + view.rect.offset.x); // blue (the format is BGRA)
                p[1] = static_cast<std::uint8_t>(y + view.rect.offset.y);
                p[2] = static_cast<std::uint8_t>(40 + view.view_index); // red
                p[3] = 0;
            }
        }
        tickets[next] = std::move(ticket);
        return next++;
    }
    ReadbackState Poll(std::uint64_t id, SubmittedPixels& pixels, std::string& error) override {
        const auto found = tickets.find(id);
        if (found == tickets.end() || found->second.finished) return ReadbackState::kNone;
        Ticket& ticket = found->second;
        if (ticket.polls_left > 0) {
            --ticket.polls_left;
            return ReadbackState::kPending;
        }
        if (ticket.fails) {
            error = "device removed";
            return ReadbackState::kFailed;
        }
        pixels.data = ticket.pixels.data();
        pixels.width = ticket.view.rect.extent.width;
        pixels.height = ticket.view.rect.extent.height;
        pixels.row_pitch = pixels.width * 4;
        pixels.format = ClassifyFormat(SubmittedImageApi::kD3D11, ticket.view.format);
        return ReadbackState::kReady;
    }
    void Finish(std::uint64_t id) override {
        const auto found = tickets.find(id);
        if (found == tickets.end()) return;
        // The pixels go: a reader still on them would read freed memory.
        found->second.finished = true;
        found->second.pixels.clear();
        found->second.pixels.shrink_to_fit();
    }
    void Reset() override { reset = true; }
    std::size_t Open() const {
        std::size_t open = 0;
        for (const auto& [id, ticket] : tickets) open += ticket.finished ? 0 : 1;
        return open;
    }
};

std::vector<SubmittedView> FakeViews() {
    std::vector<SubmittedView> views(2);
    for (std::uint32_t i = 0; i < 2; ++i) {
        views[i].resolved = true;
        views[i].swapchain = Handle(1);
        views[i].view_index = i;
        views[i].rect = {{static_cast<int>(i) * 100, 5}, {64 + static_cast<int>(i) * 8, 48}};
        views[i].format = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        views[i].width = 400;
        views[i].height = 100;
    }
    return views;
}

std::optional<ScreenshotResult> PollUntilDone(ScreenshotCapture& capture, SubmittedImageGpu* gpu, int* frames = nullptr) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    for (int frame = 0; std::chrono::steady_clock::now() < deadline; ++frame) {
        if (auto result = capture.Poll(gpu)) {
            if (frames) *frames = frame;
            return result;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return std::nullopt;
}

void TestCapture() {
    const std::filesystem::path folder = ScratchFolder(L"capture") / L"Game";
    FakeGpu gpu;
    ScreenshotCapture capture;
    std::string error;
    const std::vector<SubmittedView> views = FakeViews();
    Expect(!capture.Busy() && !capture.Poll(&gpu), "Nothing is in flight before a press");
    Expect(capture.Begin(gpu, views, SelectScreenshotEyes(2, ScreenshotEyes::Both), folder, "Game.exe", error),
           "A press starts: " + error);
    Expect(capture.Busy() && gpu.tickets.size() == 2, "Both eyes are read back");
    Expect(!capture.Poll(&gpu) && !capture.Poll(&gpu) && capture.Busy(), "Pending readbacks are only polled");
    const std::optional<ScreenshotResult> result = PollUntilDone(capture, &gpu);
    Expect(result && result->saved && result->error.empty() && result->files.size() == 2, "The press ends with two files");
    Expect(result->width == 64 && result->height == 48, "The result names the left image's size");
    Expect(result->files.size() == 2 && result->files[0].filename().wstring().find(L"_L.png") != std::wstring::npos,
           "The left eye's file is named first, whichever was written first");
    Expect(!capture.Busy() && gpu.Open() == 0, "Every ticket is ended once its pixels were converted");
    Expect(!capture.Poll(&gpu), "The outcome is reported once");
    bool left = false, right = false;
    for (const std::filesystem::path& file : result->files) {
        const std::wstring name = file.filename().wstring();
        Expect(file.parent_path() == folder && name.rfind(L"Game_", 0) == 0, "Files go to the folder under the game's name");
        const bool is_left = name.size() > 6 && name.substr(name.size() - 6) == L"_L.png";
        const bool is_right = name.size() > 6 && name.substr(name.size() - 6) == L"_R.png";
        left = left || is_left;
        right = right || is_right;
        std::vector<std::uint8_t> rgba;
        std::uint32_t width = 0, height = 0;
        Expect(ReadPng(file, rgba, width, height, error), "A saved file reads back: " + error);
        const std::uint32_t index = is_right ? 1 : 0;
        Expect(width == 64 + index * 8 && height == 48, "Each eye's file has its rectangle's size");
        bool same = true;
        for (std::uint32_t y = 0; y < height; ++y) {
            for (std::uint32_t x = 0; x < width; ++x) {
                const std::uint8_t* p = &rgba[(static_cast<std::size_t>(y) * width + x) * 4];
                same = same && p[0] == 40 + index && p[1] == y + 5 && p[2] == static_cast<std::uint8_t>(x + index * 100) &&
                       p[3] == 255;
            }
        }
        Expect(same, "Each eye's file holds its own pixels, red and blue in place");
    }
    Expect(left && right, "One file ends in _L, the other in _R");

    // One eye only.
    Expect(capture.Begin(gpu, views, SelectScreenshotEyes(2, ScreenshotEyes::Right), folder, "Game.exe", error),
           "A right-eye press starts");
    const auto right_only = PollUntilDone(capture, &gpu);
    Expect(right_only && right_only->saved && right_only->files.size() == 1 &&
               right_only->files[0].filename().wstring().find(L"_R.png") != std::wstring::npos && right_only->width == 72,
           "A right-eye press saves one _R file");

    // A readback that cannot start ends the ones started.
    gpu.fail_readback_of_view = 1;
    const std::size_t before = gpu.tickets.size();
    Expect(!capture.Begin(gpu, views, SelectScreenshotEyes(2, ScreenshotEyes::Both), folder, "Game.exe", error) &&
               error.find("no memory") != std::string::npos && !capture.Busy() && gpu.tickets.size() == before + 1 &&
               gpu.Open() == 0,
           "A readback that cannot start refuses the press and ends the one started");
    gpu.fail_readback_of_view = -1;
    // An unresolved view refuses the press.
    std::vector<SubmittedView> bad = views;
    bad[1].resolved = false;
    bad[1].problem = "its rectangle is outside";
    Expect(!capture.Begin(gpu, bad, SelectScreenshotEyes(2, ScreenshotEyes::Both), folder, "Game.exe", error) &&
               error.find("outside") != std::string::npos && gpu.Open() == 0,
           "An unresolved view refuses the press");
    Expect(capture.Begin(gpu, bad, SelectScreenshotEyes(2, ScreenshotEyes::Left), folder, "Game.exe", error),
           "...but not a press that does not save it");
    Expect(PollUntilDone(capture, &gpu).has_value(), "That press ends");
    Expect(!capture.Begin(gpu, views, {}, folder, "Game.exe", error), "A press with no view is refused");

    // A readback the GPU loses: the other eye is still saved, the press fails.
    gpu.fail_poll_of_view = 0;
    Expect(capture.Begin(gpu, views, SelectScreenshotEyes(2, ScreenshotEyes::Both), folder, "Game.exe", error),
           "A press starts before its readback fails");
    const auto failed = PollUntilDone(capture, &gpu);
    Expect(failed && !failed->saved && failed->error.find("device removed") != std::string::npos &&
               failed->files.size() == 1 && gpu.Open() == 0,
           "A failed readback fails the press, names the error and keeps the eye that was saved");
    gpu.fail_poll_of_view = -1;

    // A file that cannot be written (its folder's name is a file).
    const std::filesystem::path blocked = ScratchFolder(L"blocked");
    {
        HANDLE file = CreateFileW((blocked / L"Game").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        Expect(file != INVALID_HANDLE_VALUE, "The blocking file is created");
        CloseHandle(file);
    }
    Expect(capture.Begin(gpu, views, SelectScreenshotEyes(2, ScreenshotEyes::Left), blocked / L"Game", "Game.exe", error),
           "A press into a folder that cannot exist starts");
    const auto unwritten = PollUntilDone(capture, &gpu);
    Expect(unwritten && !unwritten->saved && !unwritten->error.empty() && unwritten->files.empty() && gpu.Open() == 0,
           "A file that cannot be written fails the press");

    // Teardown mid-press: tickets end, and the next session's press works.
    gpu.pending_polls = 1000000;
    Expect(capture.Begin(gpu, views, SelectScreenshotEyes(2, ScreenshotEyes::Both), folder, "Game.exe", error),
           "A press starts before the session ends");
    capture.Poll(&gpu);
    capture.Stop(&gpu);
    Expect(!capture.Busy() && gpu.Open() == 0 && !capture.Poll(&gpu), "Stopping ends the press and its tickets, with no outcome");
    gpu.pending_polls = 0;
    Expect(capture.Begin(gpu, views, SelectScreenshotEyes(2, ScreenshotEyes::Both), folder, "Game.exe", error),
           "A press starts again after a stop");
    // Stopped while the worker may be converting or writing: nothing is read after the tickets end.
    capture.Poll(&gpu);
    capture.Stop(&gpu);
    Expect(gpu.Open() == 0 && !capture.Busy(), "Stopping waits for the worker's reads before ending the tickets");
    Expect(capture.Begin(gpu, views, SelectScreenshotEyes(2, ScreenshotEyes::Both), folder, "Game.exe", error),
           "A press after a stop mid-write starts");
    const auto after = PollUntilDone(capture, &gpu);
    Expect(after && after->saved && after->files.size() == 2, "...and saves both eyes, counting none of the stopped press's");
    capture.Stop(nullptr);
}

// Presses in quick succession each save a picture: a few are taken while the
// ones before are still being saved, and one more than that is refused
// without anything being read back for it.
void TestCaptureQueuesPresses() {
    const std::filesystem::path folder = ScratchFolder(L"capture-queue") / L"Game";
    FakeGpu gpu;
    gpu.pending_polls = 3;
    ScreenshotCapture capture;
    std::string error;
    const std::vector<SubmittedView> views = FakeViews();
    const std::vector<ScreenshotEye> both = SelectScreenshotEyes(2, ScreenshotEyes::Both);
    Expect(kMaxScreenshotPresses >= 2 && kScreenshotWorkers >= 1, "More than one press is taken at a time");
    Expect(ScreenshotCapture::PixelBytes(SubmittedImageApi::kD3D11, views, both) == (64u * 48u + 72u * 48u) * 4u,
           "A press's size is its views' rectangles in the image's format");
    std::vector<SubmittedView> half = views;
    half[0].format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    Expect(ScreenshotCapture::PixelBytes(SubmittedImageApi::kD3D11, half, SelectScreenshotEyes(2, ScreenshotEyes::Left)) ==
               64u * 48u * 8u,
           "Half-float texels are eight bytes each");
    // A stereo press of the size the fix was asked for at, four times over.
    std::vector<SubmittedView> large = views;
    for (SubmittedView& view : large) view.rect.extent = {4880, 4124};
    const std::uint64_t large_bytes = ScreenshotCapture::PixelBytes(SubmittedImageApi::kD3D11, large, both);
    Expect(large_bytes * kMaxScreenshotPresses <= kMaxScreenshotPendingBytes &&
               large_bytes * (kMaxScreenshotPresses + 2) > kMaxScreenshotPendingBytes,
           "Four stereo presses of 4880x4124 fit the memory a queue may hold, and not many more");
    Expect(capture.Room(kMaxScreenshotPendingBytes * 4), "A first press is taken whatever its size");

    std::vector<std::uint64_t> numbers;
    for (std::size_t i = 0; i < kMaxScreenshotPresses; ++i) {
        std::uint64_t number = 0;
        Expect(capture.Begin(gpu, views, both, folder, "Game.exe", error, &number), "A press in quick succession starts: " + error);
        numbers.push_back(number);
        // A frame goes by: file names carry the time to the millisecond.
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
    }
    Expect(gpu.tickets.size() == 2 * kMaxScreenshotPresses && std::set<std::uint64_t>(numbers.begin(), numbers.end()).size() == numbers.size() &&
               numbers.front() != 0,
           "Each press is read back and has a number of its own");
    Expect(!capture.Room(1) && !capture.Begin(gpu, views, both, folder, "Game.exe", error) &&
               error.find("still being saved") != std::string::npos && gpu.tickets.size() == 2 * kMaxScreenshotPresses,
           "One press more than may be saved at a time is refused, reading nothing: " + error);
    std::set<std::uint64_t> ended;
    std::set<std::filesystem::path> files;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    bool room_back = false;
    while (ended.size() < numbers.size() && std::chrono::steady_clock::now() < deadline) {
        if (const auto result = capture.Poll(&gpu)) {
            Expect(result->saved && result->files.size() == 2 && result->width == 64, "A queued press is saved whole: " + result->error);
            Expect(ended.insert(result->press).second, "A press's outcome is reported once");
            files.insert(result->files.begin(), result->files.end());
            room_back = room_back || capture.Room(1);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    Expect(ended == std::set<std::uint64_t>(numbers.begin(), numbers.end()), "Every queued press ends with its own number");
    Expect(files.size() == 2 * kMaxScreenshotPresses, "Each press has files of its own");
    for (const std::filesystem::path& file : files) {
        std::vector<std::uint8_t> rgba;
        std::uint32_t width = 0, height = 0;
        Expect(ReadPng(file, rgba, width, height, error) && height == 48, "A queued press's file reads back: " + error);
    }
    Expect(room_back && !capture.Busy() && gpu.Open() == 0 && capture.Room(1) && !capture.Poll(&gpu),
           "With the presses saved there is room again and nothing is left open");

    // Memory: a press that would take the pixels read back past the limit
    // is refused while another is in flight.
    gpu.pending_polls = 1000000;
    Expect(capture.Begin(gpu, views, both, folder, "Game.exe", error), "A press starts: " + error);
    std::vector<SubmittedView> huge = views;
    for (SubmittedView& view : huge) view.rect.extent = {16000, 16000};
    const std::size_t tickets = gpu.tickets.size();
    Expect(!capture.Room(ScreenshotCapture::PixelBytes(SubmittedImageApi::kD3D11, huge, both)) &&
               !capture.Begin(gpu, huge, both, folder, "Game.exe", error) && gpu.tickets.size() == tickets,
           "A press too large for the memory left is refused, reading nothing");
    Expect(capture.Begin(gpu, views, both, folder, "Game.exe", error), "...while one that fits is taken");
    // Teardown with several in flight: every ticket ends, nothing is reported.
    capture.Poll(&gpu);
    capture.Stop(&gpu);
    Expect(!capture.Busy() && gpu.Open() == 0 && !capture.Poll(&gpu) && capture.Room(kMaxScreenshotPendingBytes),
           "Stopping ends every press in flight and its tickets");
    gpu.pending_polls = 0;
    Expect(capture.Begin(gpu, views, both, folder, "Game.exe", error), "A press after the stop starts");
    const auto after = PollUntilDone(capture, &gpu);
    Expect(after && after->saved && after->files.size() == 2, "...and is saved");
    capture.Stop(nullptr);
}

// The folder is named on the worker thread (asking Windows for the Pictures
// folder can take a while): a press never waits for it on the frame thread.
void TestCaptureNamesItsFolderOnTheWorker() {
    const std::filesystem::path folder = ScratchFolder(L"capture-worker") / L"Game";
    FakeGpu gpu;
    gpu.pending_polls = 0;
    ScreenshotCapture capture;
    std::string error;
    const std::vector<SubmittedView> views = FakeViews();
    std::mutex mutex;
    std::condition_variable released;
    bool release = false;
    std::atomic<int> asked{0};
    std::thread::id asked_on;
    const std::thread::id frame_thread = std::this_thread::get_id();
    const ScreenshotFolderSource slow = [&] {
        std::unique_lock lock(mutex);
        asked_on = std::this_thread::get_id();
        ++asked;
        released.wait(lock, [&] { return release; });
        return folder;
    };
    Expect(capture.Begin(gpu, views, SelectScreenshotEyes(2, ScreenshotEyes::Both), slow, "Game.exe", error),
           "A press starts while its folder is not known: " + error);
    Expect(asked == 0, "The folder is not asked for when the press starts");
    // Frames go by while the folder is being named.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (asked == 0 && std::chrono::steady_clock::now() < deadline) {
        Expect(!capture.Poll(&gpu), "The press is not over while its folder is unknown");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    for (int frame = 0; frame < 20; ++frame) {
        Expect(!capture.Poll(&gpu) && capture.Busy(), "Frames go on while the folder is being named");
    }
    {
        std::scoped_lock lock(mutex);
        Expect(asked == 1 && asked_on != frame_thread, "The folder is asked for on the worker thread");
        release = true;
    }
    released.notify_all();
    const auto result = PollUntilDone(capture, &gpu);
    Expect(result && result->saved && result->files.size() == 2 && asked == 1,
           "Both eyes are saved, the folder asked for once for the press");
    for (const std::filesystem::path& file : result->files) {
        Expect(file.parent_path() == folder && std::filesystem::exists(file), "The files are in the folder named");
    }
    // The next press asks again (the setting may have changed).
    release = true;
    Expect(capture.Begin(gpu, views, SelectScreenshotEyes(2, ScreenshotEyes::Left), slow, "Game.exe", error),
           "The next press starts");
    const auto next = PollUntilDone(capture, &gpu);
    Expect(next && next->saved && asked == 2, "Each press asks for its folder");
    // No folder at all, and a source that throws: the press fails, nothing more.
    Expect(capture.Begin(gpu, views, SelectScreenshotEyes(2, ScreenshotEyes::Left),
                         ScreenshotFolderSource([] { return std::filesystem::path(); }), "Game.exe", error),
           "A press without a folder starts");
    const auto nowhere = PollUntilDone(capture, &gpu);
    Expect(nowhere && !nowhere->saved && nowhere->files.empty() && !nowhere->error.empty() && gpu.Open() == 0,
           "A press with no folder to save to fails");
    Expect(capture.Begin(gpu, views, SelectScreenshotEyes(2, ScreenshotEyes::Left),
                         ScreenshotFolderSource([]() -> std::filesystem::path { throw std::runtime_error("no"); }),
                         "Game.exe", error),
           "A press whose folder cannot be named starts");
    const auto thrown = PollUntilDone(capture, &gpu);
    Expect(thrown && !thrown->saved && gpu.Open() == 0, "A folder that cannot be named fails the press");
    Expect(!capture.Begin(gpu, views, SelectScreenshotEyes(2, ScreenshotEyes::Left), ScreenshotFolderSource(), "Game.exe",
                          error) &&
               !capture.Busy(),
           "A press with no folder source is refused");
    capture.Stop(nullptr);
}

// A worker thread Windows will not start refuses the press: nothing is thrown
// into the game's xrEndFrame and nothing is left being read back.
void TestCaptureWithoutAWorkerThread() {
    struct NoThread final : ScreenshotCapture {
        bool fail{true};
        int starts{0};
        int allowed{1000}; // threads Windows will start
        void StartWorker() override {
            ++starts;
            if (fail || allowed <= 0) {
                throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
            }
            --allowed;
            ScreenshotCapture::StartWorker();
        }
    };
    const std::filesystem::path folder = ScratchFolder(L"capture-no-thread") / L"Game";
    FakeGpu gpu;
    gpu.pending_polls = 0;
    NoThread capture;
    std::string error;
    const std::vector<SubmittedView> views = FakeViews();
    bool thrown = false;
    bool begun = true;
    try {
        begun = capture.Begin(gpu, views, SelectScreenshotEyes(2, ScreenshotEyes::Both), folder, "Game.exe", error);
    } catch (...) {
        thrown = true;
    }
    Expect(!thrown, "A thread that cannot be started must not throw out of the press");
    Expect(!begun && error.find("could not be started") != std::string::npos, "The press is refused, with the reason: " + error);
    Expect(!capture.Busy() && gpu.tickets.empty() && !capture.Poll(&gpu), "Nothing was read back for the refused press");
    // Once a thread can be had, presses work.
    capture.fail = false;
    Expect(capture.Begin(gpu, views, SelectScreenshotEyes(2, ScreenshotEyes::Both), folder, "Game.exe", error),
           "The next press starts: " + error);
    const auto result = PollUntilDone(capture, &gpu);
    const int workers = static_cast<int>(kScreenshotWorkers);
    Expect(result && result->saved && result->files.size() == 2 && capture.starts == 1 + workers, "...and is saved");
    Expect(capture.Begin(gpu, views, SelectScreenshotEyes(2, ScreenshotEyes::Left), folder, "Game.exe", error) &&
               capture.starts == 1 + workers,
           "The worker threads are started once");
    Expect(PollUntilDone(capture, &gpu).has_value(), "That press ends");
    capture.Stop(nullptr);

    // Only one thread to be had: presses are saved by it, and the second is
    // asked for again with each press.
    NoThread single;
    single.fail = false;
    single.allowed = 1;
    Expect(single.Begin(gpu, views, SelectScreenshotEyes(2, ScreenshotEyes::Both), folder, "Game.exe", error),
           "A press starts with one worker thread: " + error);
    const auto alone = PollUntilDone(single, &gpu);
    Expect(alone && alone->saved && alone->files.size() == 2 && single.starts == (workers > 1 ? 2 : 1),
           "One worker thread saves both eyes");
    single.allowed = 1;
    std::this_thread::sleep_for(std::chrono::milliseconds(3));
    Expect(single.Begin(gpu, views, SelectScreenshotEyes(2, ScreenshotEyes::Both), folder, "Game.exe", error) &&
               single.starts == (workers > 1 ? 3 : 1),
           "The thread that could not be started is asked for with the next press");
    const auto together = PollUntilDone(single, &gpu);
    Expect(together && together->saved && together->files.size() == 2, "...and that press is saved");
    single.Stop(nullptr);
}

// Nothing thrown while a file is written leaves the worker thread (which
// would end the game): the press fails and the next one works.
void TestCaptureWorkerCatchesEverything() {
    struct Throwing final : ScreenshotCapture {
        static bool ThrowsStd(const std::filesystem::path&, const std::uint8_t*, std::uint32_t, std::uint32_t,
                              std::string&) {
            throw std::filesystem::filesystem_error("no such drive", std::make_error_code(std::errc::no_such_device));
        }
        static bool ThrowsOther(const std::filesystem::path&, const std::uint8_t*, std::uint32_t, std::uint32_t,
                                std::string&) {
            throw 42;
        }
        static bool BadAlloc(const std::filesystem::path&, const std::uint8_t*, std::uint32_t, std::uint32_t,
                             std::string&) {
            throw std::bad_alloc();
        }
        void Writer(PngWriter writer) { png_writer_ = writer; }
    };
    const std::filesystem::path folder = ScratchFolder(L"capture-throws") / L"Game";
    FakeGpu gpu;
    gpu.pending_polls = 0;
    Throwing capture;
    std::string error;
    const std::vector<SubmittedView> views = FakeViews();
    const auto press = [&](Throwing::PngWriter writer, const char* what) {
        // Between presses: the worker waits for a job, and reads the writer
        // under no lock only while it has one.
        capture.Writer(writer);
        Expect(capture.Begin(gpu, views, SelectScreenshotEyes(2, ScreenshotEyes::Both), folder, "Game.exe", error),
               std::string("A press starts (") + what + "): " + error);
        return PollUntilDone(capture, &gpu);
    };
    const auto std_thrown = press(&Throwing::ThrowsStd, "std::exception");
    Expect(std_thrown && !std_thrown->saved && std_thrown->files.empty() &&
               std_thrown->error.find("no such drive") != std::string::npos && gpu.Open() == 0,
           "A std::exception while writing fails the press with its text");
    const auto other_thrown = press(&Throwing::ThrowsOther, "anything else");
    Expect(other_thrown && !other_thrown->saved && !other_thrown->error.empty() && gpu.Open() == 0,
           "Anything else thrown while writing fails the press");
    const auto memory = press(&Throwing::BadAlloc, "bad_alloc");
    Expect(memory && !memory->saved && memory->error.find("memory") != std::string::npos && gpu.Open() == 0,
           "Running out of memory while writing fails the press");
    const auto fine = press(nullptr, "the real writer");
    Expect(fine && fine->saved && fine->files.size() == 2, "The worker is still there: the next press is saved");
    capture.Stop(nullptr);
}

// ---- WARP ----

std::uint8_t Pattern(std::uint32_t x, std::uint32_t y, std::uint32_t slice, std::uint32_t channel) {
    return static_cast<std::uint8_t>(x * 3 + y * 5 + slice * 101 + channel * 17);
}

SubmittedView View(XrSwapchain swapchain, std::int64_t format, std::uint32_t width, std::uint32_t height,
                   std::uint32_t array_size, std::uint32_t slice, XrRect2Di rect, std::uint32_t samples = 1) {
    SubmittedView view;
    view.resolved = true;
    view.swapchain = swapchain;
    view.array_slice = slice;
    view.rect = rect;
    view.format = format;
    view.width = width;
    view.height = height;
    view.array_size = array_size;
    view.sample_count = samples;
    return view;
}

// Polls as a frame loop would (never blocking in the module), until ready.
SubmittedPixels WaitReady(SubmittedImageGpu& gpu, std::uint64_t ticket, int* pending_polls = nullptr) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    SubmittedPixels pixels;
    std::string error;
    int pending = 0;
    for (;;) {
        const ReadbackState state = gpu.Poll(ticket, pixels, error);
        if (state == ReadbackState::kReady) break;
        Expect(state == ReadbackState::kPending, "A readback fails: " + error);
        Expect(std::chrono::steady_clock::now() < deadline, "A readback never finishes");
        ++pending;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (pending_polls) *pending_polls = pending;
    return pixels;
}

void ExpectPattern(const SubmittedPixels& pixels, XrRect2Di rect, std::uint32_t slice, const std::string& what) {
    Expect(pixels.width == static_cast<std::uint32_t>(rect.extent.width) &&
               pixels.height == static_cast<std::uint32_t>(rect.extent.height) && pixels.row_pitch >= pixels.width * 4,
           what + ": the readback has the rectangle's size");
    bool same = true;
    for (std::uint32_t y = 0; y < pixels.height && same; ++y) {
        const std::uint8_t* row = pixels.data + static_cast<std::size_t>(y) * pixels.row_pitch;
        for (std::uint32_t x = 0; x < pixels.width && same; ++x) {
            for (std::uint32_t c = 0; c < 4; ++c) {
                same = same && row[x * 4 + c] == Pattern(x + rect.offset.x, y + rect.offset.y, slice, c);
            }
        }
    }
    Expect(same, what + ": the readback holds the rectangle's texels of its slice");
}

std::vector<std::uint8_t> PatternImage(std::uint32_t width, std::uint32_t height, std::uint32_t slice) {
    std::vector<std::uint8_t> data(static_cast<std::size_t>(width) * height * 4);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            for (std::uint32_t c = 0; c < 4; ++c) data[(static_cast<std::size_t>(y) * width + x) * 4 + c] = Pattern(x, y, slice, c);
        }
    }
    return data;
}

void ExpectRefused(SubmittedImageGpu& gpu, const SubmittedView& view, const std::string& what) {
    std::string error;
    Expect(gpu.Readback(view, error) == 0 && !error.empty(), what + " is refused");
}

int D3D11Tests() {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    Expect(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &device,
                                       nullptr, &context)),
           "A Direct3D 11 WARP device is created");
    XrGraphicsBindingD3D11KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    binding.device = device.Get();
    std::string error;
    std::unique_ptr<SubmittedImageGpu> gpu = CreateSubmittedImageGpu(&binding, error);
    Expect(gpu && gpu->Api() == SubmittedImageApi::kD3D11 && std::string(gpu->Name()) == "D3D11" &&
               gpu->RequiredSourceUsage() == 0,
           "A Direct3D 11 binding gets the Direct3D 11 backend, which needs no usage: " + error);

    // A typeless two-slice array created as a runtime would for an sRGB swapchain.
    const std::uint32_t width = 300, height = 200;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 2;
    desc.Format = DXGI_FORMAT_R8G8B8A8_TYPELESS;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> images[2];
    for (std::uint32_t image = 0; image < 2; ++image) {
        Expect(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &images[image])), "A typeless array texture is created");
        for (std::uint32_t slice = 0; slice < 2; ++slice) {
            const std::vector<std::uint8_t> data = PatternImage(width, height, image * 2 + slice);
            context->UpdateSubresource(images[image].Get(), D3D11CalcSubresource(0, slice, 1), nullptr, data.data(), width * 4, 0);
        }
    }
    XrSwapchainImageD3D11KHR listed[2] = {{XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR, nullptr, images[0].Get()},
                                          {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR, nullptr, images[1].Get()}};
    const XrSwapchain swapchain = Handle(0x11);
    Expect(!gpu->Tracked(swapchain), "Nothing is tracked at first");
    ExpectRefused(*gpu, View(swapchain, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, width, height, 2, 0, {{0, 0}, {8, 8}}), "An untracked swapchain");
    Expect(gpu->Track(swapchain, reinterpret_cast<const XrSwapchainImageBaseHeader*>(listed), 2, error) && gpu->Tracked(swapchain),
           "The swapchain's images are tracked: " + error);
    XrSwapchainImageD3D12KHR wrong{XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR};
    Expect(!gpu->Track(Handle(0x12), reinterpret_cast<const XrSwapchainImageBaseHeader*>(&wrong), 1, error) &&
               !gpu->Tracked(Handle(0x12)),
           "Another API's image structures are not tracked");

    // A sub-rectangle of slice 1 of image 1, typeless viewed as sRGB.
    const XrRect2Di rect{{37, 21}, {150, 99}};
    SubmittedView view = View(swapchain, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, width, height, 2, 1, rect);
    view.image_index = 1;
    const std::uint64_t ticket = gpu->Readback(view, error);
    Expect(ticket != 0, "A readback starts: " + error);
    SubmittedPixels pixels = WaitReady(*gpu, ticket);
    Expect(pixels.format.kind == SubmittedPixelKind::kUnorm8 && pixels.format.srgb && !pixels.format.bgra,
           "The readback names the swapchain's format, not the texture's typeless one");
    ExpectPattern(pixels, rect, 3, "Direct3D 11 sub-rectangle of an array slice");
    SubmittedPixels again;
    Expect(gpu->Poll(ticket, again, error) == ReadbackState::kReady && again.data == pixels.data,
           "A ready ticket stays mapped");
    gpu->Finish(ticket);
    Expect(gpu->Poll(ticket, again, error) == ReadbackState::kNone, "A finished ticket is gone");
    gpu->Finish(ticket); // harmless

    // Two readbacks in flight (both eyes), whole image and slice 0.
    view = View(swapchain, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, width, height, 2, 0, {{0, 0}, {static_cast<int>(width), static_cast<int>(height)}});
    SubmittedView other = View(swapchain, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, width, height, 2, 1, {{299, 199}, {1, 1}});
    const std::uint64_t first = gpu->Readback(view, error);
    const std::uint64_t second = gpu->Readback(other, error);
    Expect(first != 0 && second != 0 && first != second, "Two readbacks are in flight at once");
    ExpectPattern(WaitReady(*gpu, second), other.rect, 1, "Direct3D 11 last texel");
    ExpectPattern(WaitReady(*gpu, first), view.rect, 0, "Direct3D 11 whole image");
    gpu->Finish(first);
    gpu->Finish(second);

    // What cannot be read is refused before anything is copied.
    ExpectRefused(*gpu, View(swapchain, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, width, height, 2, 2, rect), "A slice outside the array");
    ExpectRefused(*gpu, View(swapchain, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, width, height, 2, 0, {{200, 0}, {101, 10}}), "A rectangle outside the image");
    ExpectRefused(*gpu, View(swapchain, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, width, height, 2, 0, {{0, 0}, {0, 10}}), "An empty rectangle");
    ExpectRefused(*gpu, View(swapchain, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, width, height, 2, 0, rect), "A format of another family than the image's");
    ExpectRefused(*gpu, View(swapchain, DXGI_FORMAT_D32_FLOAT, width, height, 2, 0, rect), "A format that is not read");
    view.image_index = 2;
    ExpectRefused(*gpu, view, "An image index outside the swapchain");

    // Blue-first, 10-bit and half-float images through the conversion.
    const auto solid = [&](DXGI_FORMAT format, const void* texel, std::uint32_t bytes, XrSwapchain handle) {
        D3D11_TEXTURE2D_DESC plain = desc;
        plain.Width = 16;
        plain.Height = 8;
        plain.ArraySize = 1;
        plain.Format = format;
        std::vector<std::uint8_t> data(16 * 8 * bytes);
        for (std::size_t i = 0; i < 16 * 8; ++i) std::memcpy(&data[i * bytes], texel, bytes);
        D3D11_SUBRESOURCE_DATA initial{data.data(), 16 * bytes, 0};
        ComPtr<ID3D11Texture2D> texture;
        Expect(SUCCEEDED(device->CreateTexture2D(&plain, &initial, &texture)), "A solid texture is created");
        XrSwapchainImageD3D11KHR image{XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR, nullptr, texture.Get()};
        Expect(gpu->Track(handle, reinterpret_cast<const XrSwapchainImageBaseHeader*>(&image), 1, error), "It is tracked");
        const std::uint64_t id = gpu->Readback(View(handle, format, 16, 8, 1, 0, {{3, 2}, {5, 4}}), error);
        Expect(id != 0, "Its readback starts: " + error);
        std::uint8_t rgba[5 * 4 * 4]{};
        Expect(ConvertToRgba8(WaitReady(*gpu, id), rgba), "Its pixels convert");
        gpu->Finish(id);
        gpu->Forget(handle);
        Expect(!gpu->Tracked(handle), "A forgotten swapchain is not tracked");
        return std::vector<std::uint8_t>(rgba + 19 * 4, rgba + 20 * 4);
    };
    const std::uint8_t bgra[4] = {10, 20, 30, 0};
    Expect(solid(DXGI_FORMAT_B8G8R8A8_UNORM, bgra, 4, Handle(0x21)) == std::vector<std::uint8_t>{30, 20, 10, 255},
           "A blue-first image is saved red first, opaque");
    const std::uint32_t ten = 1023u | (512u << 10) | (0u << 20);
    Expect(solid(DXGI_FORMAT_R10G10B10A2_UNORM, &ten, 4, Handle(0x22)) ==
               std::vector<std::uint8_t>{255, LinearToSrgb8(512.0f / 1023.0f), 0, 255},
           "A 10-bit image is encoded to sRGB");
    const std::uint16_t half[4] = {Half(0.5f), Half(2.0f), Half(-0.5f), Half(0.0f)};
    Expect(solid(DXGI_FORMAT_R16G16B16A16_FLOAT, half, 8, Handle(0x23)) == std::vector<std::uint8_t>{188, 255, 0, 255},
           "A half-float image is clamped and encoded to sRGB");

    // Multisampled: resolved.
    {
        D3D11_TEXTURE2D_DESC msaa = desc;
        msaa.Width = 64;
        msaa.Height = 32;
        msaa.ArraySize = 2;
        msaa.SampleDesc.Count = 4;
        msaa.BindFlags = D3D11_BIND_RENDER_TARGET;
        ComPtr<ID3D11Texture2D> texture;
        Expect(SUCCEEDED(device->CreateTexture2D(&msaa, nullptr, &texture)), "A multisampled typeless array is created");
        const float colors[2][4] = {{1.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 0.0f, 1.0f}};
        for (UINT slice = 0; slice < 2; ++slice) {
            D3D11_RENDER_TARGET_VIEW_DESC target{};
            target.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            target.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY;
            target.Texture2DMSArray.FirstArraySlice = slice;
            target.Texture2DMSArray.ArraySize = 1;
            ComPtr<ID3D11RenderTargetView> rtv;
            Expect(SUCCEEDED(device->CreateRenderTargetView(texture.Get(), &target, &rtv)), "A slice's render target is created");
            context->ClearRenderTargetView(rtv.Get(), colors[slice]);
        }
        XrSwapchainImageD3D11KHR image{XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR, nullptr, texture.Get()};
        Expect(gpu->Track(Handle(0x31), reinterpret_cast<const XrSwapchainImageBaseHeader*>(&image), 1, error), "It is tracked");
        const std::uint64_t id = gpu->Readback(View(Handle(0x31), DXGI_FORMAT_R8G8B8A8_UNORM, 64, 32, 2, 1, {{10, 4}, {20, 9}}, 4), error);
        Expect(id != 0, "A multisampled readback starts: " + error);
        const SubmittedPixels resolved = WaitReady(*gpu, id);
        Expect(resolved.width == 20 && resolved.height == 9, "The resolved readback has the rectangle's size");
        const std::uint8_t* last = resolved.data + 8 * resolved.row_pitch + 19 * 4;
        Expect(resolved.data[0] == 0 && resolved.data[1] == 255 && resolved.data[2] == 0 && last[0] == 0 && last[1] == 255,
               "A multisampled slice is resolved before it is read");
        gpu->Finish(id);
    }

    // A whole press: readback, conversion and the PNG.
    {
        const std::filesystem::path folder = ScratchFolder(L"d3d11");
        ScreenshotCapture capture;
        std::vector<SubmittedView> views = {View(swapchain, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, width, height, 2, 0, {{0, 0}, {150, 200}}),
                                            View(swapchain, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, width, height, 2, 1, {{150, 0}, {150, 200}})};
        views[1].view_index = 1;
        Expect(capture.Begin(*gpu, views, SelectScreenshotEyes(2, ScreenshotEyes::Both), folder, "warp.exe", error),
               "A press on Direct3D 11 starts: " + error);
        const auto result = PollUntilDone(capture, gpu.get());
        Expect(result && result->saved && result->files.size() == 2, "The press saves two files" + (result ? ": " + result->error : ""));
        for (const std::filesystem::path& file : result->files) {
            const bool right = file.filename().wstring().find(L"_R.png") != std::wstring::npos;
            std::vector<std::uint8_t> rgba;
            std::uint32_t w = 0, h = 0;
            Expect(ReadPng(file, rgba, w, h, error) && w == 150 && h == 200, "A saved eye reads back: " + error);
            bool same = true;
            for (std::uint32_t y = 0; y < h; ++y) {
                for (std::uint32_t x = 0; x < w; ++x) {
                    for (std::uint32_t c = 0; c < 3; ++c) {
                        same = same && rgba[(static_cast<std::size_t>(y) * w + x) * 4 + c] ==
                                           Pattern(x + (right ? 150 : 0), y, right ? 1 : 0, c);
                    }
                    same = same && rgba[(static_cast<std::size_t>(y) * w + x) * 4 + 3] == 255;
                }
            }
            Expect(same, "Each eye's PNG holds its rectangle of its slice");
        }
        capture.Stop(gpu.get());
    }

    // Teardown with a ticket in flight and one mapped.
    const std::uint64_t open = gpu->Readback(View(swapchain, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, width, height, 2, 0, rect), error);
    const std::uint64_t mapped = gpu->Readback(View(swapchain, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, width, height, 2, 0, rect), error);
    Expect(open != 0 && mapped != 0, "Readbacks start before the teardown");
    WaitReady(*gpu, mapped);
    gpu->Reset();
    Expect(!gpu->Tracked(swapchain) && gpu->Poll(open, pixels, error) == ReadbackState::kNone, "A reset forgets everything");
    gpu.reset();
    images[0].Reset();
    Expect(images[1].Reset() == 0, "Nothing of the swapchain's images is held after the teardown");

    XrGraphicsBindingD3D11KHR empty{XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    Expect(!CreateSubmittedImageGpu(&empty, error) && !error.empty(), "A binding without a device has no backend");
    // XrGraphicsBindingVulkanKHR's layout (this test is built without the Vulkan headers), with no handles.
    struct {
        XrStructureType type;
        const void* next;
        void* instance;
        void* physical_device;
        void* device;
        std::uint32_t queue_family;
        std::uint32_t queue_index;
    } vulkan{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
    Expect(!CreateSubmittedImageGpu(&vulkan, error) && error.find("Vulkan") != std::string::npos,
           "A Vulkan binding without an instance or device has no backend, and says so");
    Expect(!CreateSubmittedImageGpu(nullptr, error) && !error.empty(), "No binding, no backend");
    std::cout << "Direct3D 11 (WARP) readback: " << checks << " checks passed\n";
    return 0;
}

struct D3D12Rig {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> fence;
    std::uint64_t serial{0};

    void Run(const std::function<void(ID3D12GraphicsCommandList*)>& record) {
        ComPtr<ID3D12CommandAllocator> allocator;
        ComPtr<ID3D12GraphicsCommandList> commands;
        Expect(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) &&
                   SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                                       IID_PPV_ARGS(&commands))),
               "The test's command list is created");
        record(commands.Get());
        Expect(SUCCEEDED(commands->Close()), "The test's command list closes");
        ID3D12CommandList* lists[] = {commands.Get()};
        queue->ExecuteCommandLists(1, lists);
        Expect(SUCCEEDED(queue->Signal(fence.Get(), ++serial)), "The test's fence is signalled");
        const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        Expect(event && SUCCEEDED(fence->SetEventOnCompletion(serial, event)) &&
                   WaitForSingleObject(event, 30000) == WAIT_OBJECT_0,
               "The test's GPU work finishes");
        CloseHandle(event);
    }

    // A texture in RENDER_TARGET, as a released swapchain image rests.
    ComPtr<ID3D12Resource> Texture(DXGI_FORMAT format, std::uint32_t width, std::uint32_t height, std::uint16_t slices,
                                   std::uint32_t samples = 1) {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = slices;
        desc.MipLevels = 1;
        desc.Format = format;
        desc.SampleDesc.Count = samples;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        ComPtr<ID3D12Resource> texture;
        Expect(SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                         nullptr, IID_PPV_ARGS(&texture))),
               "A Direct3D 12 texture is created");
        return texture;
    }

    // Fills a slice from CPU data (`bytes` per texel), leaving it in RENDER_TARGET.
    void Fill(ID3D12Resource* texture, DXGI_FORMAT typed, std::uint32_t slice, const std::uint8_t* data, std::uint32_t bytes) {
        const D3D12_RESOURCE_DESC desc = texture->GetDesc();
        D3D12_RESOURCE_DESC one = desc;
        one.DepthOrArraySize = 1;
        one.Format = typed;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        UINT64 total = 0;
        device->GetCopyableFootprints(&one, 0, 1, 0, &footprint, nullptr, nullptr, &total);
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC buffer{};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = total;
        buffer.Height = 1;
        buffer.DepthOrArraySize = 1;
        buffer.MipLevels = 1;
        buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> upload;
        Expect(SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                         nullptr, IID_PPV_ARGS(&upload))),
               "The test's upload buffer is created");
        std::uint8_t* mapped = nullptr;
        Expect(SUCCEEDED(upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped))), "The upload buffer maps");
        const std::size_t row = static_cast<std::size_t>(desc.Width) * bytes;
        for (std::uint32_t y = 0; y < desc.Height; ++y) {
            std::memcpy(mapped + static_cast<std::size_t>(y) * footprint.Footprint.RowPitch, data + y * row, row);
        }
        upload->Unmap(0, nullptr);
        footprint.Footprint.Format = desc.Format;
        Run([&](ID3D12GraphicsCommandList* commands) {
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition = {texture, slice, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_DEST};
            commands->ResourceBarrier(1, &barrier);
            D3D12_TEXTURE_COPY_LOCATION to{};
            to.pResource = texture;
            to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            to.SubresourceIndex = slice;
            D3D12_TEXTURE_COPY_LOCATION from{};
            from.pResource = upload.Get();
            from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            from.PlacedFootprint = footprint;
            commands->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
            std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
            commands->ResourceBarrier(1, &barrier);
        });
    }
};

int D3D12Tests() {
    D3D12Rig rig;
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter> warp;
    Expect(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) && SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp))),
           "The WARP adapter is found");
    Expect(SUCCEEDED(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&rig.device))),
           "A Direct3D 12 WARP device is created");
    D3D12_COMMAND_QUEUE_DESC queue{};
    Expect(SUCCEEDED(rig.device->CreateCommandQueue(&queue, IID_PPV_ARGS(&rig.queue))) &&
               SUCCEEDED(rig.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&rig.fence))),
           "The session's queue is created");
    XrGraphicsBindingD3D12KHR binding{XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};
    binding.device = rig.device.Get();
    binding.queue = rig.queue.Get();
    std::string error;
    std::unique_ptr<SubmittedImageGpu> gpu = CreateSubmittedImageGpu(&binding, error);
    Expect(gpu && gpu->Api() == SubmittedImageApi::kD3D12 && std::string(gpu->Name()) == "D3D12" &&
               gpu->RequiredSourceUsage() == 0,
           "A Direct3D 12 binding gets the Direct3D 12 backend, which needs no usage: " + error);

    // A typeless two-slice array, as a runtime creates an sRGB swapchain's.
    const std::uint32_t width = 300, height = 200;
    ComPtr<ID3D12Resource> images[2];
    for (std::uint32_t image = 0; image < 2; ++image) {
        images[image] = rig.Texture(DXGI_FORMAT_R8G8B8A8_TYPELESS, width, height, 2);
        for (std::uint32_t slice = 0; slice < 2; ++slice) {
            const std::vector<std::uint8_t> data = PatternImage(width, height, image * 2 + slice);
            rig.Fill(images[image].Get(), DXGI_FORMAT_R8G8B8A8_UNORM, slice, data.data(), 4);
        }
    }
    XrSwapchainImageD3D12KHR listed[2] = {{XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR, nullptr, images[0].Get()},
                                          {XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR, nullptr, images[1].Get()}};
    const XrSwapchain swapchain = Handle(0x11);
    ExpectRefused(*gpu, View(swapchain, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, width, height, 2, 0, {{0, 0}, {8, 8}}), "An untracked swapchain");
    // Tracked through the runtime's enumeration, as the layer does.
    static XrSwapchainImageD3D12KHR* enumerated = nullptr;
    enumerated = listed;
    const PFN_xrEnumerateSwapchainImages enumerate = [](XrSwapchain, std::uint32_t capacity, std::uint32_t* count,
                                                        XrSwapchainImageBaseHeader* out) -> XrResult {
        *count = 2;
        if (capacity == 0) return XR_SUCCESS;
        if (capacity < 2 || out->type != XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR) return XR_ERROR_VALIDATION_FAILURE;
        auto* typed = reinterpret_cast<XrSwapchainImageD3D12KHR*>(out);
        typed[0].texture = enumerated[0].texture;
        typed[1].texture = enumerated[1].texture;
        return XR_SUCCESS;
    };
    Expect(TrackSubmittedSwapchain(*gpu, swapchain, enumerate, error) && gpu->Tracked(swapchain),
           "The swapchain is tracked from the runtime's enumeration: " + error);
    Expect(!TrackSubmittedSwapchain(*gpu, Handle(0x12), nullptr, error) && !gpu->Tracked(Handle(0x12)),
           "Without an enumeration nothing is tracked");
    void* textures[1] = {nullptr};
    Expect(!TrackSubmittedSwapchainD3D11(*gpu, Handle(0x12), textures, 1, error), "Direct3D 11 textures are not this backend's");

    const XrRect2Di rect{{37, 21}, {150, 99}};
    SubmittedView view = View(swapchain, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, width, height, 2, 1, rect);
    view.image_index = 1;
    const std::uint64_t ticket = gpu->Readback(view, error);
    Expect(ticket != 0, "A readback starts: " + error);
    SubmittedPixels pixels = WaitReady(*gpu, ticket);
    Expect(pixels.format.kind == SubmittedPixelKind::kUnorm8 && pixels.format.srgb,
           "The readback names the swapchain's format, not the texture's typeless one");
    ExpectPattern(pixels, rect, 3, "Direct3D 12 sub-rectangle of an array slice");
    gpu->Finish(ticket);
    SubmittedPixels gone;
    Expect(gpu->Poll(ticket, gone, error) == ReadbackState::kNone, "A finished ticket is gone");

    // The image is back in RENDER_TARGET: the same read again, and the other
    // slice untouched, with two tickets in flight.
    SubmittedView whole = View(swapchain, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, width, height, 2, 0, {{0, 0}, {static_cast<int>(width), static_cast<int>(height)}});
    whole.image_index = 1;
    const std::uint64_t first = gpu->Readback(whole, error);
    const std::uint64_t second = gpu->Readback(view, error);
    Expect(first != 0 && second != 0 && first != second, "Two readbacks are in flight at once: " + error);
    ExpectPattern(WaitReady(*gpu, second), rect, 3, "Direct3D 12 the same rectangle again");
    ExpectPattern(WaitReady(*gpu, first), whole.rect, 2, "Direct3D 12 whole image of slice 0");
    gpu->Finish(second);
    gpu->Finish(first);
    // A row pitch that is not the row's width (300 texels are 1200 bytes; the pitch is aligned to 256).
    SubmittedView narrow = View(swapchain, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, width, height, 2, 0, {{299, 199}, {1, 1}});
    const std::uint64_t corner = gpu->Readback(narrow, error);
    ExpectPattern(WaitReady(*gpu, corner), narrow.rect, 0, "Direct3D 12 last texel");
    gpu->Finish(corner);

    ExpectRefused(*gpu, View(swapchain, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, width, height, 2, 2, rect), "A slice outside the array");
    ExpectRefused(*gpu, View(swapchain, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, width, height, 2, 0, {{200, 0}, {101, 10}}), "A rectangle outside the image");
    ExpectRefused(*gpu, View(swapchain, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB, width, height, 2, 0, rect), "A format of another family than the image's");
    ExpectRefused(*gpu, View(swapchain, DXGI_FORMAT_D32_FLOAT, width, height, 2, 0, rect), "A format that is not read");
    view.image_index = 2;
    ExpectRefused(*gpu, view, "An image index outside the swapchain");

    const auto solid = [&](DXGI_FORMAT format, const void* texel, std::uint32_t bytes, XrSwapchain handle) {
        ComPtr<ID3D12Resource> texture = rig.Texture(format, 16, 8, 1);
        std::vector<std::uint8_t> data(16 * 8 * bytes);
        for (std::size_t i = 0; i < 16 * 8; ++i) std::memcpy(&data[i * bytes], texel, bytes);
        rig.Fill(texture.Get(), format, 0, data.data(), bytes);
        XrSwapchainImageD3D12KHR image{XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR, nullptr, texture.Get()};
        Expect(gpu->Track(handle, reinterpret_cast<const XrSwapchainImageBaseHeader*>(&image), 1, error), "It is tracked");
        const std::uint64_t id = gpu->Readback(View(handle, format, 16, 8, 1, 0, {{3, 2}, {5, 4}}), error);
        Expect(id != 0, "Its readback starts: " + error);
        std::uint8_t rgba[5 * 4 * 4]{};
        Expect(ConvertToRgba8(WaitReady(*gpu, id), rgba), "Its pixels convert");
        gpu->Finish(id);
        gpu->Forget(handle);
        return std::vector<std::uint8_t>(rgba + 19 * 4, rgba + 20 * 4);
    };
    const std::uint8_t bgra[4] = {10, 20, 30, 0};
    Expect(solid(DXGI_FORMAT_B8G8R8A8_UNORM, bgra, 4, Handle(0x21)) == std::vector<std::uint8_t>{30, 20, 10, 255},
           "A blue-first image is saved red first, opaque");
    const std::uint32_t ten = 1023u | (512u << 10) | (0u << 20);
    Expect(solid(DXGI_FORMAT_R10G10B10A2_UNORM, &ten, 4, Handle(0x22)) ==
               std::vector<std::uint8_t>{255, LinearToSrgb8(512.0f / 1023.0f), 0, 255},
           "A 10-bit image is encoded to sRGB");
    const std::uint16_t half[4] = {Half(0.5f), Half(2.0f), Half(-0.5f), Half(0.0f)};
    Expect(solid(DXGI_FORMAT_R16G16B16A16_FLOAT, half, 8, Handle(0x23)) == std::vector<std::uint8_t>{188, 255, 0, 255},
           "A half-float image is clamped and encoded to sRGB");

    // Multisampled: resolved.
    {
        ComPtr<ID3D12Resource> texture = rig.Texture(DXGI_FORMAT_R8G8B8A8_TYPELESS, 64, 32, 2, 4);
        D3D12_DESCRIPTOR_HEAP_DESC heap{};
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heap.NumDescriptors = 2;
        ComPtr<ID3D12DescriptorHeap> targets;
        Expect(SUCCEEDED(rig.device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&targets))), "A render target heap is created");
        const UINT step = rig.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        rig.Run([&](ID3D12GraphicsCommandList* commands) {
            const float colors[2][4] = {{1.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 1.0f, 0.0f, 1.0f}};
            for (UINT slice = 0; slice < 2; ++slice) {
                D3D12_RENDER_TARGET_VIEW_DESC target{};
                target.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                target.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY;
                target.Texture2DMSArray.FirstArraySlice = slice;
                target.Texture2DMSArray.ArraySize = 1;
                D3D12_CPU_DESCRIPTOR_HANDLE handle = targets->GetCPUDescriptorHandleForHeapStart();
                handle.ptr += slice * step;
                rig.device->CreateRenderTargetView(texture.Get(), &target, handle);
                commands->ClearRenderTargetView(handle, colors[slice], 0, nullptr);
            }
        });
        XrSwapchainImageD3D12KHR image{XR_TYPE_SWAPCHAIN_IMAGE_D3D12_KHR, nullptr, texture.Get()};
        Expect(gpu->Track(Handle(0x31), reinterpret_cast<const XrSwapchainImageBaseHeader*>(&image), 1, error), "It is tracked");
        const std::uint64_t id = gpu->Readback(View(Handle(0x31), DXGI_FORMAT_R8G8B8A8_UNORM, 64, 32, 2, 1, {{10, 4}, {20, 9}}, 4), error);
        Expect(id != 0, "A multisampled readback starts: " + error);
        const SubmittedPixels resolved = WaitReady(*gpu, id);
        const std::uint8_t* last = resolved.data + 8 * resolved.row_pitch + 19 * 4;
        Expect(resolved.width == 20 && resolved.height == 9 && resolved.data[0] == 0 && resolved.data[1] == 255 &&
                   resolved.data[2] == 0 && last[0] == 0 && last[1] == 255,
               "A multisampled slice is resolved before it is read");
        gpu->Finish(id);
        // The swapchain destroyed while a copy of its image is in flight: the image outlives the copy.
        const std::uint64_t late = gpu->Readback(View(Handle(0x31), DXGI_FORMAT_R8G8B8A8_UNORM, 64, 32, 2, 0, {{0, 0}, {64, 32}}, 4), error);
        Expect(late != 0, "A readback starts before its swapchain goes");
        gpu->Forget(Handle(0x31));
        texture.Reset();
        const SubmittedPixels kept = WaitReady(*gpu, late);
        Expect(kept.data[0] == 255 && kept.data[1] == 0, "A copy in flight keeps its image until it is done");
        gpu->Finish(late);
    }

    // A whole press: readback, conversion and the PNG.
    {
        const std::filesystem::path folder = ScratchFolder(L"d3d12");
        ScreenshotCapture capture;
        std::vector<SubmittedView> views = {View(swapchain, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, width, height, 2, 0, {{0, 0}, {150, 200}}),
                                            View(swapchain, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, width, height, 2, 1, {{150, 0}, {150, 200}})};
        views[1].view_index = 1;
        Expect(capture.Begin(*gpu, views, SelectScreenshotEyes(2, ScreenshotEyes::Both), folder, "warp.exe", error),
               "A press on Direct3D 12 starts: " + error);
        const auto result = PollUntilDone(capture, gpu.get());
        Expect(result && result->saved && result->files.size() == 2, "The press saves two files" + (result ? ": " + result->error : ""));
        for (const std::filesystem::path& file : result->files) {
            const bool right = file.filename().wstring().find(L"_R.png") != std::wstring::npos;
            std::vector<std::uint8_t> rgba;
            std::uint32_t w = 0, h = 0;
            Expect(ReadPng(file, rgba, w, h, error) && w == 150 && h == 200, "A saved eye reads back: " + error);
            bool same = true;
            for (std::uint32_t y = 0; y < h; ++y) {
                for (std::uint32_t x = 0; x < w; ++x) {
                    for (std::uint32_t c = 0; c < 3; ++c) {
                        same = same && rgba[(static_cast<std::size_t>(y) * w + x) * 4 + c] ==
                                           Pattern(x + (right ? 150 : 0), y, right ? 1 : 0, c);
                    }
                }
            }
            Expect(same, "Each eye's PNG holds its rectangle of its slice");
        }
        capture.Stop(gpu.get());
    }

    // Teardown with tickets in flight, mapped and finished early.
    const std::uint64_t open = gpu->Readback(whole, error);
    const std::uint64_t mapped = gpu->Readback(whole, error);
    const std::uint64_t early = gpu->Readback(whole, error);
    Expect(open != 0 && mapped != 0 && early != 0, "Readbacks start before the teardown");
    gpu->Finish(early); // before the GPU may be done: kept until it is
    WaitReady(*gpu, mapped);
    gpu->Reset();
    Expect(!gpu->Tracked(swapchain) && gpu->Poll(open, pixels, error) == ReadbackState::kNone, "A reset forgets everything");
    gpu.reset();
    images[0].Reset();
    Expect(images[1].Reset() == 0, "Nothing of the swapchain's images is held after the teardown");

    XrGraphicsBindingD3D12KHR empty{XR_TYPE_GRAPHICS_BINDING_D3D12_KHR};
    Expect(!CreateSubmittedImageGpu(&empty, error) && !error.empty(), "A binding without a device or queue has no backend");
    std::cout << "Direct3D 12 (WARP) readback: " << checks << " checks passed\n";
    return 0;
}

// By hand: how long presses of a real picture take to save, with one worker
// thread (as before presses were queued) and with kScreenshotWorkers.
// `source`: a PNG (a saved screenshot), used for both eyes; the files go to
// the scratch folder, which is removed afterwards.
struct PictureGpu final : FakeGpu {
    const std::vector<std::uint8_t>* picture{nullptr};
    std::uint32_t width{0}, height{0};
    std::uint64_t Readback(const SubmittedView& view, std::string&) override {
        Ticket ticket;
        ticket.view = view;
        tickets[next] = std::move(ticket);
        return next++;
    }
    ReadbackState Poll(std::uint64_t id, SubmittedPixels& pixels, std::string&) override {
        const auto found = tickets.find(id);
        if (found == tickets.end() || found->second.finished) return ReadbackState::kNone;
        pixels.data = picture->data();
        pixels.width = width;
        pixels.height = height;
        pixels.row_pitch = width * 4;
        pixels.format = ClassifyFormat(SubmittedImageApi::kD3D11, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
        return ReadbackState::kReady;
    }
};

int CaptureBench(const std::filesystem::path& source) {
    struct Limited final : ScreenshotCapture {
        explicit Limited(std::size_t workers) { worker_limit_ = workers; }
    };
    std::vector<std::uint8_t> rgba;
    PictureGpu gpu;
    std::string error;
    if (!ReadPng(source, rgba, gpu.width, gpu.height, error)) {
        std::cerr << "Reading the picture failed: " << error << '\n';
        return 1;
    }
    gpu.picture = &rgba;
    std::cout << "Both eyes " << gpu.width << "x" << gpu.height << ", files written to the temporary folder\n";
    std::vector<SubmittedView> views = FakeViews();
    for (SubmittedView& view : views) {
        view.rect = {{0, 0}, {static_cast<int>(gpu.width), static_cast<int>(gpu.height)}};
        view.format = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    }
    const std::vector<ScreenshotEye> both = SelectScreenshotEyes(2, ScreenshotEyes::Both);
    for (const std::size_t workers : {std::size_t{1}, kScreenshotWorkers}) {
        for (const std::size_t presses : {std::size_t{1}, kMaxScreenshotPresses}) {
            const std::filesystem::path folder =
                ScratchFolder(L"capture-bench") / (std::to_wstring(workers) + L"-" + std::to_wstring(presses));
            Limited capture(workers);
            const auto begun = std::chrono::steady_clock::now();
            const auto elapsed = [&] {
                return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - begun).count();
            };
            for (std::size_t press = 0; press < presses; ++press) {
                if (!capture.Begin(gpu, views, both, folder, "Game.exe", error)) {
                    std::cerr << "A press was refused: " << error << '\n';
                    return 1;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(3));
            }
            std::size_t ended = 0;
            double first = 0.0;
            while (ended < presses) {
                if (const auto result = capture.Poll(&gpu)) {
                    if (!result->saved) {
                        std::cerr << "A press failed: " << result->error << '\n';
                        return 1;
                    }
                    if (ended++ == 0) first = elapsed();
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            std::cout << workers << " worker thread" << (workers == 1 ? "" : "s") << ", " << presses << " stereo press"
                      << (presses == 1 ? "" : "es") << ": first saved after " << first << " ms, all after " << elapsed()
                      << " ms" << std::endl;
            capture.Stop(nullptr);
        }
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "";
    // The PNG encoder's COM, for the tests that write and read files on this thread.
    Expect(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)), "COM starts");
    int result = 0;
    if (mode == "d3d11") {
        result = D3D11Tests();
    } else if (mode == "d3d12") {
        result = D3D12Tests();
    } else if (mode == "capture-bench" && argc > 2) {
        // By hand: CPU only.
        result = CaptureBench(std::filesystem::path(argv[2]));
    } else if (mode.empty()) {
        TestFormats();
        TestPixelConversion();
        TestNames();
        TestEyes();
        TestBindingPress();
        TestPngRoundTrip();
        TestPngLimits();
        TestViewResolution();
        TestSettings();
        TestCapture();
        TestCaptureQueuesPresses();
        TestCaptureNamesItsFolderOnTheWorker();
        TestCaptureWithoutAWorkerThread();
        TestCaptureWorkerCatchesEverything();
        std::cout << "Imaging and screenshots (CPU): " << checks << " checks passed\n";
    } else {
        std::cerr << "Unknown mode " << mode << '\n';
        result = 2;
    }
    RemoveScratch();
    CoUninitialize();
    return result;
}
