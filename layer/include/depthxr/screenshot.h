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
    // A quad views eye that was not composed into one image (the runtime
    // composes the four views, or Frame Generation does): saved as a picture
    // of `width` x `height` with its focus view drawn over the eye's own
    // view (SubmittedImageGpu::ReadbackComposed).
    bool composed{false};
    SubmittedFocus focus;
    std::uint32_t width{0};
    std::uint32_t height{0};
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
    std::uint64_t press{0}; // the number Begin gave it
    bool saved{false};
    std::vector<std::filesystem::path> files;
    std::string error;
    std::uint32_t width{0}; // of the first file
    std::uint32_t height{0};
};

// Presses in quick succession each save a picture: a press is taken while
// earlier ones are still being read back, converted or written, up to this
// many at a time...
inline constexpr std::size_t kMaxScreenshotPresses = 4;
// ...and while the pixels read back for them and not yet handed to a worker
// (a view's rectangle in the image's own format, in memory the GPU's copy
// went to) stay within this. A stereo press of 4880x4124 8-bit views is 161
// MB, so four of those fit; a first press is taken whatever its size. Each
// worker holds one more picture as 8-bit RGBA while it encodes it (80 MB at
// that size), which is not counted here.
inline constexpr std::uint64_t kMaxScreenshotPendingBytes = 768ull << 20;
// The threads that convert and encode: a press's two eyes are written side
// by side (the PNG encoder takes about a second for a 20-megapixel view,
// whichever of PNG's row filters it is given).
inline constexpr std::size_t kScreenshotWorkers = 2;

// One session's screenshots. Begin, Poll and Stop are the frame thread's
// (with the game's Direct3D 11 context held where there is one); nothing in
// them waits for the GPU, for a file or for the shell, and nothing in them
// throws.
class ScreenshotCapture {
  public:
    ScreenshotCapture();
    virtual ~ScreenshotCapture();

    // A press being read back, converted or written. Takes no lock.
    bool Busy() const;
    // The bytes a press of `eyes` of `views` reads back.
    static std::uint64_t PixelBytes(SubmittedImageApi api, const std::vector<SubmittedView>& views,
                                    const std::vector<ScreenshotEye>& eyes);
    // Whether another press of that many bytes is taken now
    // (kMaxScreenshotPresses, kMaxScreenshotPendingBytes). Takes no lock.
    bool Room(std::uint64_t pixel_bytes) const;
    // Starts reading back `eyes` of `views`. False with `error` when there
    // is no room for the press or nothing could be started (a worker thread
    // that cannot be started included). `folder` is asked for on a worker
    // thread. `press`: the number its outcome carries.
    bool Begin(SubmittedImageGpu& gpu, const std::vector<SubmittedView>& views, const std::vector<ScreenshotEye>& eyes,
               ScreenshotFolderSource folder, const std::string& exe_name, std::string& error,
               std::uint64_t* press = nullptr);
    // With the folder known already.
    bool Begin(SubmittedImageGpu& gpu, const std::vector<SubmittedView>& views, const std::vector<ScreenshotEye>& eyes,
               const std::filesystem::path& folder, const std::string& exe_name, std::string& error,
               std::uint64_t* press = nullptr);
    // Every frame: moves readbacks the GPU finished to the workers, and ends
    // those a worker has read. Returns one press's outcome, once, when its
    // files are written or it failed (a call for each press that ended).
    // With no press in flight it takes no lock.
    std::optional<ScreenshotResult> Poll(SubmittedImageGpu* gpu);
    // Teardown: waits for the workers to stop reading mapped pixels (not for
    // files being encoded from their own copies), ends the tickets and drops
    // every press. The worker threads stay for the next session.
    void Stop(SubmittedImageGpu* gpu);

  protected:
    // Starts one worker thread; may throw (std::system_error).
    virtual void StartWorker();
    void WorkerLoop();
    // Tests: what the worker writes a file with instead of WritePng (set
    // before the first press).
    using PngWriter = bool (*)(const std::filesystem::path&, const std::uint8_t*, std::uint32_t, std::uint32_t,
                               std::string&);
    PngWriter png_writer_{nullptr};
    // Tests and measurements: fewer worker threads than kScreenshotWorkers
    // (set before the first press).
    std::size_t worker_limit_{kScreenshotWorkers};

  private:
    struct Shot {
        std::uint64_t ticket{0};
        std::string file_name;
        enum class Stage { kReadback, kConverting, kConverted, kDone } stage{Stage::kReadback};
        SubmittedPixels pixels;
        std::uint64_t bytes{0}; // counted in pending_bytes_ until its ticket ends
    };
    // Where a press's files go: asked for once, by the first worker that
    // writes one of them.
    struct Folder {
        std::mutex mutex;
        ScreenshotFolderSource source;
        bool asked{false};
        std::filesystem::path path;
    };
    struct Press {
        std::uint64_t number{0};
        std::vector<Shot> shots;
        std::shared_ptr<Folder> folder;
        std::size_t written{0};
        ScreenshotResult result;
    };
    struct Job {
        std::uint64_t press{0};
        std::size_t shot{0};
        SubmittedPixels pixels;
        std::string file_name;
        std::shared_ptr<Folder> folder;
    };
    bool EnsureWorkers(std::string& error);
    Press* FindPressLocked(std::uint64_t number);
    void EndTicketLocked(SubmittedImageGpu* gpu, Shot& shot);

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable converted_;
    std::vector<std::thread> workers_;
    bool stop_{false};
    std::deque<Job> jobs_;
    // The presses in flight, oldest first (frame thread, under mutex_ where
    // a worker reads them).
    std::deque<Press> presses_;
    std::atomic<std::size_t> in_flight_{0};         // presses_.size(), read without the lock
    std::atomic<std::uint64_t> pending_bytes_{0};   // of tickets not ended yet
    std::uint64_t last_press_{0};
    std::size_t converting_{0}; // jobs whose mapped pixels a worker still reads
};

} // namespace depthxr
