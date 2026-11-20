#pragma once

// Full-resolution screenshots of the images a game submits: which views a
// press saves, where the files go, the PNG encoder, and the capture object
// that carries a press from the frame thread (GPU readbacks that never wait,
// submitted_image.h) to a worker thread (pixel conversion, PNG, the file).

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "depthxr/settings.h"
#include "depthxr/submitted_image.h"

namespace depthxr {

class Logger;

// A press is the binding going down after a poll that saw it up; the first
// poll only primes (a binding held as the session starts is no press).
struct ScreenshotBindingEdge {
    bool primed{false};
    bool was_down{false};
    bool Update(bool down) {
        const bool pressed = primed && down && !was_down;
        primed = true;
        was_down = down;
        return pressed;
    }
    void Reset() { primed = false; was_down = false; }
};

struct ScreenshotEye {
    std::uint32_t view_index{0};
    char letter{'L'};
};
// The views of a frame's projection layer a press saves: view 0 as L and
// view 1 as R (a quad views layer's first two views are its stereo views),
// narrowed to the eye asked for; a single view is saved as L whatever was
// asked. Empty for no views.
std::vector<ScreenshotEye> SelectScreenshotEyes(std::uint32_t view_count, ScreenshotEyes eyes);

struct ScreenshotTime {
    int year{0}, month{0}, day{0}, hour{0}, minute{0}, second{0}, millisecond{0};
};
ScreenshotTime ScreenshotLocalTime();
// The game's folder and file stem: its program file's name without the
// extension, characters a file name cannot hold replaced by '_' ("game" when
// nothing is left).
std::string ScreenshotExeStem(const std::string& exe_name);
// "<exe-stem>_<yyyyMMdd_HHmmss_mmm>_<L|R>.png".
std::string ScreenshotFileName(const std::string& exe_name, const ScreenshotTime& time, char eye);
// "<custom or default root>/<exe-stem>". The default root is VectorXR in the
// user's Pictures folder (`pictures`; when that is empty, screenshots under
// VectorXR in `local_app_data`). A custom folder that is not absolute is not
// used.
std::filesystem::path ScreenshotFolder(const std::string& custom_folder, const std::string& exe_name,
                                       const std::filesystem::path& pictures,
                                       const std::filesystem::path& local_app_data);
// Whether `custom_folder` is used as the root: it names an absolute folder.
bool ScreenshotFolderIsCustom(const std::string& custom_folder);
// The user's Pictures folder (empty when Windows names none). Asks the
// shell, which loads shell32.dll the first time and may take a while (a
// redirected or offline folder): never called on the thread that ends the
// game's frames.
std::filesystem::path PicturesFolder();
// Where a press's files go. Called on the worker thread, once for a press.
using ScreenshotFolderSource = std::function<std::filesystem::path()>;

// Writes 8-bit RGBA pixels (alpha ignored: the file is opaque) as a PNG, to a
// temporary file beside `path` moved over it when complete. Uses the Windows
// Imaging Component: the calling thread must have COM initialized.
bool WritePng(const std::filesystem::path& path, const std::uint8_t* rgba, std::uint32_t width, std::uint32_t height,
              std::string& error);
// Reads a PNG as 8-bit RGBA (alpha not premultiplied). Its size is read from
// its header before anything is decoded: one wider or taller than
// kMaxPngSide, or of more than kMaxPngPixels, is refused. With `max_side`,
// a larger picture is shrunk to fit it while it is decoded, so it is never
// held whole. Never throws.
inline constexpr std::uint32_t kMaxPngSide = 16384;
inline constexpr std::uint64_t kMaxPngPixels = 8192ull * 8192ull;
bool ReadPng(const std::filesystem::path& path, std::vector<std::uint8_t>& rgba, std::uint32_t& width,
             std::uint32_t& height, std::string& error, std::uint32_t max_side = 0);

// What became of a press.
struct ScreenshotResult {
    bool saved{false};
    std::vector<std::filesystem::path> files;
    std::string error;
    std::uint32_t width{0}; // of the first file
    std::uint32_t height{0};
};

// One session's screenshots. Begin, Poll and Stop are the frame thread's
// (with the game's Direct3D 11 context held where there is one); nothing in
// them waits for the GPU, for a file or for the shell, and nothing in them
// throws.
class ScreenshotCapture {
  public:
    ScreenshotCapture();
    virtual ~ScreenshotCapture();

    // A press being read back, converted or written: another is refused.
    // Takes no lock.
    bool Busy() const;
    // Starts reading back `eyes` of `views`. False with `error` when busy or
    // nothing could be started (a worker thread that cannot be started
    // included). `folder` is asked for on the worker thread.
    bool Begin(SubmittedImageGpu& gpu, const std::vector<SubmittedView>& views, const std::vector<ScreenshotEye>& eyes,
               ScreenshotFolderSource folder, const std::string& exe_name, std::string& error);
    // With the folder known already.
    bool Begin(SubmittedImageGpu& gpu, const std::vector<SubmittedView>& views, const std::vector<ScreenshotEye>& eyes,
               const std::filesystem::path& folder, const std::string& exe_name, std::string& error);
    // Every frame: moves readbacks the GPU finished to the worker, and ends
    // those the worker has read. Returns a press's outcome once, when its
    // files are written or it failed. With no press in flight it takes no
    // lock.
    std::optional<ScreenshotResult> Poll(SubmittedImageGpu* gpu);
    // Teardown: waits for the worker to stop reading mapped pixels (not for
    // files being encoded from its own copy), ends the tickets and drops the
    // press. The worker thread stays for the next session.
    void Stop(SubmittedImageGpu* gpu);

  protected:
    // Starts the worker thread; may throw (std::system_error).
    virtual void StartWorker();
    void WorkerLoop();
    // Tests: what the worker writes a file with instead of WritePng (set
    // before the first press).
    using PngWriter = bool (*)(const std::filesystem::path&, const std::uint8_t*, std::uint32_t, std::uint32_t,
                               std::string&);
    PngWriter png_writer_{nullptr};

  private:
    struct Shot {
        std::uint64_t ticket{0};
        std::string file_name;
        enum class Stage { kReadback, kConverting, kConverted, kDone } stage{Stage::kReadback};
        SubmittedPixels pixels;
    };
    struct Job {
        std::uint64_t press{0};
        std::size_t shot{0};
        SubmittedPixels pixels;
        std::string file_name;
    };
    bool EnsureWorker(std::string& error);

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable converted_;
    std::thread worker_;
    bool stop_{false};
    std::deque<Job> jobs_;
    // The press in flight (frame thread, under mutex_ where the worker reads it).
    bool active_{false};
    std::atomic<bool> busy_{false}; // `active_`, read without the lock
    ScreenshotFolderSource folder_; // the press's
    std::uint64_t press_{0};
    std::vector<Shot> shots_;
    std::size_t written_{0};
    std::size_t converting_{0}; // jobs whose mapped pixels the worker still reads
    ScreenshotResult result_;
};

} // namespace depthxr
