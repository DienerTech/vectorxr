#include <windows.h>
#include <shlobj.h>
#include <wincodec.h>
#include <wrl/client.h>

#include "depthxr/screenshot.h"

#include "depthxr/atomic_file.h"
#include "depthxr/config_path.h"

#include <algorithm>
#include <cstdio>
#include <new>
#include <system_error>

namespace depthxr {
using Microsoft::WRL::ComPtr;
namespace {

std::string Failure(const char* operation, HRESULT result) {
    char code[16];
    sprintf_s(code, "0x%08lX", static_cast<unsigned long>(result));
    return std::string(operation) + " failed (" + code + ")";
}

std::filesystem::path Utf8Path(const std::string& text) {
    if (text.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(std::max(length, 0)), L'\0');
    if (length > 0) MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), wide.data(), length);
    return std::filesystem::path(wide);
}

} // namespace

std::vector<ScreenshotEye> SelectScreenshotEyes(std::uint32_t view_count, ScreenshotEyes eyes) {
    std::vector<ScreenshotEye> selected;
    if (view_count == 0) return selected;
    if (view_count == 1) {
        selected.push_back({0, 'L'});
        return selected;
    }
    if (eyes != ScreenshotEyes::Right) selected.push_back({0, 'L'});
    if (eyes != ScreenshotEyes::Left) selected.push_back({1, 'R'});
    return selected;
}

ScreenshotTime ScreenshotLocalTime() {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    return {now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond, now.wMilliseconds};
}

std::string ScreenshotExeStem(const std::string& exe_name) {
    std::string stem = exe_name;
    // Its last path component, without one extension.
    if (const auto slash = stem.find_last_of("\\/"); slash != std::string::npos) stem.erase(0, slash + 1);
    if (const auto dot = stem.find_last_of('.'); dot != std::string::npos && dot > 0) stem.erase(dot);
    for (char& c : stem) {
        const auto u = static_cast<unsigned char>(c);
        if (u < 0x20 || c == '<' || c == '>' || c == ':' || c == '"' || c == '/' || c == '\\' || c == '|' || c == '?' ||
            c == '*') {
            c = '_';
        }
    }
    while (!stem.empty() && (stem.back() == ' ' || stem.back() == '.')) stem.pop_back();
    while (!stem.empty() && stem.front() == ' ') stem.erase(stem.begin());
    return stem.empty() ? std::string("game") : stem;
}

std::string ScreenshotFileName(const std::string& exe_name, const ScreenshotTime& time, char eye) {
    char stamp[40];
    sprintf_s(stamp, "_%04d%02d%02d_%02d%02d%02d_%03d_%c.png", time.year, time.month, time.day, time.hour, time.minute,
              time.second, time.millisecond, eye);
    return ScreenshotExeStem(exe_name) + stamp;
}

std::filesystem::path ScreenshotFolder(const std::string& custom_folder, const std::string& exe_name,
                                       const std::filesystem::path& pictures,
                                       const std::filesystem::path& local_app_data) {
    std::filesystem::path root = Utf8Path(custom_folder);
    if (!ScreenshotFolderIsCustom(custom_folder)) {
        root = !pictures.empty() ? pictures / L"VectorXR" : local_app_data / L"VectorXR" / L"screenshots";
    }
    return root / Utf8Path(ScreenshotExeStem(exe_name));
}

bool ScreenshotFolderIsCustom(const std::string& custom_folder) {
    return Utf8Path(custom_folder).is_absolute();
}

std::filesystem::path PicturesFolder() {
    // shell32.dll is loaded with the first screenshot, not with the layer:
    // a game that takes none never gets it from here.
    using KnownFolder = HRESULT(WINAPI*)(REFKNOWNFOLDERID, DWORD, HANDLE, PWSTR*);
    static const KnownFolder known_folder = [] {
        const HMODULE shell = LoadLibraryExW(L"shell32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        return shell ? reinterpret_cast<KnownFolder>(GetProcAddress(shell, "SHGetKnownFolderPath")) : nullptr;
    }();
    PWSTR folder = nullptr;
    std::filesystem::path path;
    if (known_folder && SUCCEEDED(known_folder(FOLDERID_Pictures, KF_FLAG_DEFAULT, nullptr, &folder)) && folder) {
        path = folder;
    }
    if (folder) CoTaskMemFree(folder);
    if (path.empty()) {
        const std::filesystem::path profile = EnvironmentPath(L"USERPROFILE");
        if (!profile.empty()) path = profile / L"Pictures";
    }
    return path;
}

bool WritePng(const std::filesystem::path& path, const std::uint8_t* rgba, std::uint32_t width, std::uint32_t height,
              std::string& error) {
    if (!rgba || width == 0 || height == 0) {
        error = "No pixels to write";
        return false;
    }
    std::error_code created;
    std::filesystem::create_directories(path.parent_path(), created);
    if (created) {
        error = "Creating " + PathToUtf8(path.parent_path()) + " failed: " + created.message();
        return false;
    }
    const std::filesystem::path temporary = UniqueTemporaryPath(path);
    const auto fail = [&](const char* operation, HRESULT result) {
        error = Failure(operation, result);
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        return false;
    };
    {
        ComPtr<IWICImagingFactory> factory;
        HRESULT result = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
        if (FAILED(result)) return fail("Creating the imaging factory", result);
        ComPtr<IWICStream> stream;
        result = factory->CreateStream(&stream);
        if (SUCCEEDED(result)) result = stream->InitializeFromFilename(temporary.c_str(), GENERIC_WRITE);
        if (FAILED(result)) return fail("Opening the file", result);
        ComPtr<IWICBitmapEncoder> encoder;
        result = factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
        if (SUCCEEDED(result)) result = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
        if (FAILED(result)) return fail("Creating the PNG encoder", result);
        ComPtr<IWICBitmapFrameEncode> frame;
        result = encoder->CreateNewFrame(&frame, nullptr);
        if (SUCCEEDED(result)) result = frame->Initialize(nullptr);
        if (SUCCEEDED(result)) result = frame->SetSize(width, height);
        WICPixelFormatGUID format = GUID_WICPixelFormat24bppBGR;
        if (SUCCEEDED(result)) result = frame->SetPixelFormat(&format);
        if (FAILED(result)) return fail("Starting the PNG frame", result);
        // The encoder may answer with another format than the one asked for.
        const bool bgr24 = IsEqualGUID(format, GUID_WICPixelFormat24bppBGR);
        const bool bgra32 = IsEqualGUID(format, GUID_WICPixelFormat32bppBGRA);
        const bool rgba32 = IsEqualGUID(format, GUID_WICPixelFormat32bppRGBA);
        if (!bgr24 && !bgra32 && !rgba32) return fail("Choosing the PNG pixel format", E_FAIL);
        const std::uint32_t bytes = bgr24 ? 3 : 4;
        const std::uint32_t stride = width * bytes;
        const std::uint32_t rows = std::clamp<std::uint32_t>((4u << 20) / stride, 1, height);
        std::vector<std::uint8_t> band(static_cast<std::size_t>(stride) * rows);
        for (std::uint32_t y = 0; y < height; y += rows) {
            const std::uint32_t count = std::min(rows, height - y);
            const std::uint8_t* in = rgba + static_cast<std::size_t>(y) * width * 4;
            std::uint8_t* out = band.data();
            for (std::size_t i = 0, n = static_cast<std::size_t>(count) * width; i < n; ++i, in += 4, out += bytes) {
                if (rgba32) {
                    out[0] = in[0];
                    out[1] = in[1];
                    out[2] = in[2];
                    out[3] = 255;
                } else {
                    out[0] = in[2];
                    out[1] = in[1];
                    out[2] = in[0];
                    if (bgra32) out[3] = 255;
                }
            }
            result = frame->WritePixels(count, stride, stride * count, band.data());
            if (FAILED(result)) return fail("Writing the PNG's pixels", result);
        }
        result = frame->Commit();
        if (SUCCEEDED(result)) result = encoder->Commit();
        if (FAILED(result)) return fail("Finishing the PNG", result);
    }
    std::uint32_t code = 0;
    if (!ReplaceFileAtomically(temporary, path, &code)) {
        error = "Moving the finished file to " + PathToUtf8(path) + " failed (error " + std::to_string(code) + ")";
        return false;
    }
    return true;
}

bool ReadPng(const std::filesystem::path& path, std::vector<std::uint8_t>& rgba, std::uint32_t& width,
             std::uint32_t& height, std::string& error, std::uint32_t max_side) {
    ComPtr<IWICImagingFactory> factory;
    HRESULT result = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    ComPtr<IWICBitmapDecoder> decoder;
    if (SUCCEEDED(result)) {
        result = factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
                                                    &decoder);
    }
    ComPtr<IWICBitmapFrameDecode> frame;
    if (SUCCEEDED(result)) result = decoder->GetFrame(0, &frame);
    // The frame's size, from its header: before a pixel of it is decoded.
    UINT w = 0, h = 0;
    if (SUCCEEDED(result)) result = frame->GetSize(&w, &h);
    if (FAILED(result)) {
        error = Failure("Reading the PNG", result);
        return false;
    }
    if (w == 0 || h == 0 || w > kMaxPngSide || h > kMaxPngSide ||
        static_cast<std::uint64_t>(w) * h > kMaxPngPixels) {
        error = "The PNG is " + std::to_string(w) + "x" + std::to_string(h) + ": larger than " +
                std::to_string(kMaxPngSide) + " a side or " + std::to_string(kMaxPngPixels >> 20) +
                " megapixels, or empty";
        return false;
    }
    // A converter object, not WICConvertBitmapSource: the layer imports
    // nothing from windowscodecs.dll (it loads with the first screenshot).
    const bool shrink = max_side > 0 && (w > max_side || h > max_side);
    ComPtr<IWICFormatConverter> converted;
    result = factory->CreateFormatConverter(&converted);
    if (SUCCEEDED(result)) {
        // Shrunk while it is decoded (WIC pulls the rows through the scaler:
        // the picture is never held whole): premultiplied, so a transparent
        // texel's colour does not bleed into its neighbours.
        result = converted->Initialize(frame.Get(),
                                       shrink ? GUID_WICPixelFormat32bppPBGRA : GUID_WICPixelFormat32bppRGBA,
                                       WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom);
    }
    ComPtr<IWICBitmapSource> source = converted;
    if (SUCCEEDED(result) && shrink) {
        const double scale = std::min(static_cast<double>(max_side) / w, static_cast<double>(max_side) / h);
        const UINT scaled_w = std::max<UINT>(1, static_cast<UINT>(w * scale));
        const UINT scaled_h = std::max<UINT>(1, static_cast<UINT>(h * scale));
        ComPtr<IWICBitmapScaler> scaler;
        result = factory->CreateBitmapScaler(&scaler);
        if (SUCCEEDED(result)) {
            result = scaler->Initialize(converted.Get(), scaled_w, scaled_h, WICBitmapInterpolationModeFant);
        }
        if (SUCCEEDED(result)) {
            source = scaler;
            w = scaled_w;
            h = scaled_h;
        }
    }
    if (SUCCEEDED(result)) {
        try {
            rgba.resize(static_cast<std::size_t>(w) * h * 4);
        } catch (const std::bad_alloc&) {
            error = "Not enough memory for the PNG";
            return false;
        }
        result = source->CopyPixels(nullptr, w * 4, static_cast<UINT>(rgba.size()), rgba.data());
    }
    if (FAILED(result)) {
        error = Failure("Reading the PNG", result);
        return false;
    }
    if (shrink) {
        // Premultiplied BGRA back to straight RGBA.
        for (std::size_t i = 0; i + 3 < rgba.size(); i += 4) {
            const std::uint32_t alpha = rgba[i + 3];
            const auto straight = [alpha](std::uint8_t value) {
                return alpha ? static_cast<std::uint8_t>(std::min<std::uint32_t>(255, (value * 255u + alpha / 2) / alpha))
                             : std::uint8_t{0};
            };
            const std::uint8_t blue = straight(rgba[i]), green = straight(rgba[i + 1]), red = straight(rgba[i + 2]);
            rgba[i] = red;
            rgba[i + 1] = green;
            rgba[i + 2] = blue;
        }
    }
    width = w;
    height = h;
    return true;
}

ScreenshotCapture::ScreenshotCapture() = default;

ScreenshotCapture::~ScreenshotCapture() {
    {
        std::scoped_lock lock(mutex_);
        stop_ = true;
        jobs_.clear();
    }
    wake_.notify_all();
    if (worker_.joinable()) worker_.join();
}

bool ScreenshotCapture::Busy() const {
    return busy_.load(std::memory_order_acquire);
}

void ScreenshotCapture::StartWorker() {
    worker_ = std::thread([this] { WorkerLoop(); });
}

bool ScreenshotCapture::EnsureWorker(std::string& error) {
    if (worker_.joinable()) return true;
    // On the thread that ends the game's frames, inside its xrEndFrame: a
    // thread Windows will not start is a refused press, not an exception in
    // the game.
    try {
        StartWorker();
    } catch (const std::exception& failure) {
        error = std::string("the thread that saves screenshots could not be started (") + failure.what() + ")";
        return false;
    } catch (...) {
        error = "the thread that saves screenshots could not be started";
        return false;
    }
    return true;
}

bool ScreenshotCapture::Begin(SubmittedImageGpu& gpu, const std::vector<SubmittedView>& views,
                              const std::vector<ScreenshotEye>& eyes, const std::filesystem::path& folder,
                              const std::string& exe_name, std::string& error) {
    return Begin(gpu, views, eyes, ScreenshotFolderSource([folder] { return folder; }), exe_name, error);
}

bool ScreenshotCapture::Begin(SubmittedImageGpu& gpu, const std::vector<SubmittedView>& views,
                              const std::vector<ScreenshotEye>& eyes, ScreenshotFolderSource folder,
                              const std::string& exe_name, std::string& error) {
    std::scoped_lock lock(mutex_);
    if (active_) {
        error = "the last screenshot is still being saved";
        return false;
    }
    if (eyes.empty()) {
        error = "the frame has no view to save";
        return false;
    }
    if (!folder) {
        error = "there is no folder to save to";
        return false;
    }
    // Before anything is read back for it.
    if (!EnsureWorker(error)) {
        return false;
    }
    const ScreenshotTime time = ScreenshotLocalTime();
    std::vector<Shot> shots;
    for (const ScreenshotEye& eye : eyes) {
        if (eye.view_index >= views.size()) {
            error = "the frame has no view " + std::to_string(eye.view_index);
            break;
        }
        if (!views[eye.view_index].resolved) {
            error = "view " + std::to_string(eye.view_index) + ": " + views[eye.view_index].problem;
            break;
        }
        Shot shot;
        shot.ticket = gpu.Readback(views[eye.view_index], error);
        if (shot.ticket == 0) {
            error = "view " + std::to_string(eye.view_index) + ": " + error;
            break;
        }
        shot.file_name = ScreenshotFileName(exe_name, time, eye.letter);
        shots.push_back(std::move(shot));
    }
    if (shots.size() != eyes.size()) {
        for (const Shot& shot : shots) gpu.Finish(shot.ticket);
        return false;
    }
    shots_ = std::move(shots);
    folder_ = std::move(folder);
    written_ = 0;
    result_ = {};
    ++press_;
    active_ = true;
    busy_.store(true, std::memory_order_release);
    return true;
}

std::optional<ScreenshotResult> ScreenshotCapture::Poll(SubmittedImageGpu* gpu) {
    // Every frame of every session: nothing in flight costs no lock.
    if (!busy_.load(std::memory_order_acquire)) return std::nullopt;
    std::unique_lock lock(mutex_);
    if (!active_) return std::nullopt;
    bool queued = false;
    bool tickets_open = false;
    for (std::size_t i = 0; i < shots_.size(); ++i) {
        Shot& shot = shots_[i];
        if (shot.stage == Shot::Stage::kReadback) {
            std::string error;
            const ReadbackState state = gpu ? gpu->Poll(shot.ticket, shot.pixels, error) : ReadbackState::kNone;
            if (state == ReadbackState::kReady) {
                shot.stage = Shot::Stage::kConverting;
                if (i == 0) {
                    result_.width = shot.pixels.width;
                    result_.height = shot.pixels.height;
                }
                jobs_.push_back({press_, i, shot.pixels, shot.file_name});
                queued = true;
            } else if (state != ReadbackState::kPending) {
                if (gpu) gpu->Finish(shot.ticket);
                shot.stage = Shot::Stage::kDone;
                ++written_;
                if (result_.error.empty()) result_.error = error.empty() ? "the readback was lost" : error;
            }
        } else if (shot.stage == Shot::Stage::kConverted) {
            // The worker has its own copy: the mapped pixels go.
            if (gpu) gpu->Finish(shot.ticket);
            shot.stage = Shot::Stage::kDone;
        }
        tickets_open = tickets_open || shot.stage != Shot::Stage::kDone;
    }
    if (queued) wake_.notify_one();
    if (tickets_open || written_ < shots_.size()) return std::nullopt;
    active_ = false;
    busy_.store(false, std::memory_order_release);
    ScreenshotResult result = std::move(result_);
    result.saved = result.error.empty() && !result.files.empty();
    shots_.clear();
    return result;
}

void ScreenshotCapture::Stop(SubmittedImageGpu* gpu) {
    std::unique_lock lock(mutex_);
    jobs_.clear();
    converted_.wait(lock, [this] { return converting_ == 0; });
    if (gpu) {
        for (const Shot& shot : shots_) {
            if (shot.stage != Shot::Stage::kDone) gpu->Finish(shot.ticket);
        }
    }
    shots_.clear();
    active_ = false;
    busy_.store(false, std::memory_order_release);
    ++press_; // what the worker still writes belongs to no press
}

void ScreenshotCapture::WorkerLoop() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    // The folder of the press being written, asked for here (Windows may
    // take its time to name the Pictures folder), once for its files.
    std::uint64_t folder_press = 0;
    std::filesystem::path folder;
    for (;;) {
        Job job;
        ScreenshotFolderSource folder_source;
        {
            std::unique_lock lock(mutex_);
            wake_.wait(lock, [this] { return stop_ || !jobs_.empty(); });
            if (stop_) break;
            job = std::move(jobs_.front());
            jobs_.pop_front();
            ++converting_;
            if (job.press != folder_press && job.press == press_) folder_source = folder_;
        }
        std::vector<std::uint8_t> rgba;
        std::string error;
        bool ok = true;
        // Nothing thrown here may leave the thread: it would end the game.
        try {
            rgba.resize(static_cast<std::size_t>(job.pixels.width) * job.pixels.height * 4);
            if (!ConvertToRgba8(job.pixels, rgba.data())) {
                ok = false;
                error = "The image's format cannot be saved";
            }
        } catch (const std::bad_alloc&) {
            ok = false;
            error = "Not enough memory for the image";
        } catch (const std::exception& failure) {
            ok = false;
            error = std::string("Converting the image failed (") + failure.what() + ")";
        } catch (...) {
            ok = false;
            error = "Converting the image failed";
        }
        {
            std::scoped_lock lock(mutex_);
            --converting_;
            if (job.press == press_ && active_ && job.shot < shots_.size()) {
                shots_[job.shot].stage = Shot::Stage::kConverted;
            }
        }
        converted_.notify_all();
        if (ok && FAILED(com)) {
            ok = false;
            error = Failure("Starting COM for the PNG encoder", com);
        }
        std::filesystem::path path;
        try {
            if (folder_source) {
                folder = folder_source();
                folder_press = job.press;
            }
            if (job.press == folder_press && !folder.empty()) path = folder / Utf8Path(job.file_name);
            if (ok && path.empty()) {
                ok = false;
                error = "There is no folder to save to";
            }
            if (ok) {
                ok = (png_writer_ ? png_writer_ : &WritePng)(path, rgba.data(), job.pixels.width, job.pixels.height,
                                                             error);
            }
        } catch (const std::bad_alloc&) {
            ok = false;
            error = "Not enough memory to write the PNG";
        } catch (const std::exception& failure) {
            ok = false;
            error = std::string("Writing the PNG failed (") + failure.what() + ")";
        } catch (...) {
            ok = false;
            error = "Writing the PNG failed";
        }
        std::scoped_lock lock(mutex_);
        if (job.press == press_ && active_) {
            ++written_;
            if (ok) {
                result_.files.push_back(path);
            } else if (result_.error.empty()) {
                result_.error = error;
            }
        }
    }
    if (SUCCEEDED(com)) CoUninitialize();
}

} // namespace depthxr
