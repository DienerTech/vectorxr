#pragma once

// Engine motion vectors (XR_FB_space_warp, XR_EXT_frame_synthesis) as Frame
// Generation takes them: one record per frame, whichever extension the game
// chained, made right after the layer takes the structures off the frame
// (spacewarp_probe.h); and stage 0's census, worked out from the backend's
// readbacks of the game's motion vector images (FrameGenGpu::CaptureMotion).
// Nothing generated frames are made from uses them yet.

#include <openxr/openxr.h>

#include "depthxr/framegen_gpu.h"
#include "depthxr/spacewarp_probe.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace depthxr {

class Logger;

enum class FrameGenMotionSource : std::uint8_t { kNone, kSpaceWarpFb, kFrameSynthesisExt };
const char* ToString(FrameGenMotionSource source); // "none", "fb", "ext"

// What ValidateEngineMotionView finds wrong with a structure (bits).
inline constexpr std::uint32_t kEngineMotionNearFar = 1u;    // nearZ equals farZ
inline constexpr std::uint32_t kEngineMotionDepthRange = 2u; // not 0 <= minDepth <= maxDepth <= 1
inline constexpr std::uint32_t kEngineMotionRect = 4u;       // an empty region, or one outside its swapchain
inline constexpr std::uint32_t kEngineMotionFormat = 8u;     // vectors neither RGBA16F, RG16F nor RGBA32F

// What the layer knows of a swapchain from its creation.
struct EngineMotionSwapchain {
    std::uint32_t width{0};
    std::uint32_t height{0};
    std::uint32_t array_size{1};
    std::int64_t format{0};
};
// The layer's swapchains by handle; none where it does not know one.
using EngineMotionSwapchains = std::function<std::optional<EngineMotionSwapchain>(XrSwapchain)>;

// RGBA16F, RG16F or RGBA32F in DXGI, Vulkan or GL.
bool IsEngineMotionFormat(std::int64_t format);

// One projection view's structure, as Frame Generation reads it.
struct FrameGenEngineMotionView {
    std::uint32_t view{0};
    FrameGenMotionSource source{FrameGenMotionSource::kNone};
    XrSwapchainSubImage vectors{};
    XrSwapchainSubImage depth{};
    XrSwapchainSubImage color{};    // the projection view's own image
    std::int64_t vectors_format{0}; // the vectors' swapchain's format (0: not known)
    FrameGenDepthEncoding depth_encoding; // the structure's minDepth, maxDepth, nearZ, farZ
    // The structure's depth is the view's XrCompositionLayerDepthInfoKHR:
    // the same swapchain, region and slice.
    bool depth_is_khr{false};
    FrameGenMotionEncoding encoding;
    std::uint64_t layer_flags{0};
    bool frame_skip{false};       // XR_FB_space_warp's FRAME_SKIP
    bool relaxed_interval{false}; // XR_EXT_frame_synthesis' REQUEST_RELAXED_FRAME_INTERVAL
    XrPosef app_delta{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 0.0f}}; // appSpaceDeltaPose
    // The pose and field of view the game rendered the view with, before the
    // layer's own changes (Pivot).
    FrameGenViewGeometry game;
    std::uint32_t problems{0}; // kEngineMotion* bits
};

// A frame's structures on its primary projection (the first with views).
struct FrameGenEngineMotionFrame {
    XrTime display_time{0};
    std::uint64_t projection_frame{0}; // the layer's count of frames that carried a projection layer (0: none here)
    std::uint32_t primary_layer{0};
    std::vector<FrameGenEngineMotionView> views; // in view order
    bool complete{false};     // every view carried a structure, of one extension
    std::uint32_t invalid{0}; // structures with problems
};

// The record of a frame the layer took structures off (`storage`, from
// WithoutSpaceWarp) and the frame it forwards (`stripped`); none when the
// primary projection carried no structure the layer took. The game's poses
// are its views' in `stripped`, by layer index (the copies in `storage` are
// only of the projections that carried structures). `swapchains` tells
// the regions and formats checked; without it they are not.
std::optional<FrameGenEngineMotionFrame> ToEngineMotionFrame(const SpaceWarpFrame& storage, const XrFrameEndInfo& stripped,
                                                             std::uint64_t projection_frame,
                                                             const EngineMotionSwapchains& swapchains = {});
// kEngineMotion* bits for a structure: nearZ != farZ, 0 <= minDepth <=
// maxDepth <= 1, and with its swapchain known, a region inside it and a
// format for vectors.
std::uint32_t EngineMotionProblems(const SpaceWarpViewInfo& view, const std::optional<EngineMotionSwapchain>& vectors);
bool ValidateEngineMotionView(const SpaceWarpViewInfo& view, std::string& why,
                              const std::optional<EngineMotionSwapchain>& vectors = std::nullopt);

// Stage 0 (VECTORXR_FRAMEGEN_ENGINE_MV=census): what the census of a game's
// motion vector images shows, worked out on a thread of its own from each
// frame's record and the backend's readbacks (a few frames later):
//   which image is written (FrameGenEngineImageTracker) and how many
//     change, are exactly zero or not finite;
//   whether the written image is complete when the frame's depth is
//     released (early) and untouched after xrEndFrame (end against the next
//     frame's early);
//   the online fit of cockpit vectors (nearer than 4 m) against the head's
//     movement, e = a * p_r + b * p_t + c per axis, with the newest pair of
//     poses and with the pair before (lag), and the spread across frames of
//     each frame's intercept under that fit (jitter, in vector pixels once
//     a fit gives the gain).
// A line every 30 s; with a CSV path, a row per view and frame for 20 minutes.
// The game's frames only queue what they hand over (NoteFrame, NoteStatus)
// and read the trackers (Image, Summarize): the work, the CSV and the log
// are the worker thread's alone.
class FrameGenEngineCensusWorker {
  public:
    // A frame's record as the presenter had it at the frame's end.
    struct Frame {
        FrameGenEngineMotionFrame record;
        // Per view: the images its color and depth swapchains released for
        // it, the vectors' swapchain's release since the frame before (a
        // game following the specification), and the color view's size.
        std::vector<std::uint32_t> color_index;
        std::vector<std::uint32_t> depth_index;
        std::vector<std::optional<std::uint32_t>> released;
        std::vector<FrameGenExtent> color_extent;
        // The record of the frame before was kept: no pause, abandoned frame
        // or new session came between.
        bool continuous{false};
    };

    FrameGenEngineCensusWorker(Logger* logger, std::string csv_path,
                               std::chrono::milliseconds summary_interval = std::chrono::seconds(30));
    ~FrameGenEngineCensusWorker();
    FrameGenEngineCensusWorker(const FrameGenEngineCensusWorker&) = delete;
    FrameGenEngineCensusWorker& operator=(const FrameGenEngineCensusWorker&) = delete;
    // Process teardown: the thread stops on its own, never joined (the
    // worker must then outlive it: it is never destroyed).
    void Abandon();

    void NoteFrame(Frame frame);
    // The backend's state: its counts, and the censuses read since the last
    // call (by sequence).
    void NoteStatus(const FrameGenEngineStatus& status);
    // The image `view`'s vectors are thought to be in for a frame (see
    // FrameGenEngineImageTracker::Image).
    std::optional<std::uint32_t> Image(std::size_t view, std::uint64_t frame, std::uint32_t color_index,
                                       std::uint32_t depth_index, std::optional<std::uint32_t> released) const;
    // What a research capture's motion.txt says of a view: the policy the
    // tracker holds best, and the newest census at a frame's end.
    struct ViewSummary {
        FrameGenEngineImageTracker::Hypothesis best;
        bool locked{false};
        std::uint64_t census_frame{0};
        std::uint32_t images{0};
        std::array<std::uint64_t, kFrameGenEngineImages> hashes{};
    };
    std::optional<ViewSummary> Summarize(std::size_t view) const;
    // Tests: waits until everything noted so far is worked out, then the
    // summary line as it would be logged now.
    std::string Flush();

  private:
    // An image's censuses at frames' ends in the window: of informative
    // frames, and how many of those it changed in; its hash grid's exact
    // zeros and non-finite texels, of all its texels read.
    struct ImageWindow {
        std::uint64_t informative{0};
        std::uint64_t changed{0};
        std::uint64_t zeros{0};
        std::uint64_t non_finite{0};
        std::uint64_t texels{0};
    };
    struct Comparison {
        std::uint64_t equal{0};
        std::uint64_t compared{0};
    };
    // A view's censuses by frame, end and early, while they may still meet.
    struct Hashes {
        std::optional<std::array<std::uint64_t, kFrameGenEngineImages>> end;
        std::optional<std::array<std::uint64_t, kFrameGenEngineImages>> early;
        std::optional<std::uint32_t> written; // the one image that changed at its end
        bool compared_early{false};          // early against end
        bool compared_next{false};           // end against the next frame's early
    };
    // A frame's cockpit samples, summed, and its vectors' half extent in
    // pixels: its intercept under a fit, for the jitter.
    struct FrameSamples {
        std::array<FrameGenEngineAxisFit, 2> fit{};
        std::array<double, 2> pixels{};
    };
    using AxisSolutions = std::array<FrameGenEngineAxisFit::Solution, 2>;
    struct ViewState {
        // Read by the game's frames too (Image, Summarize): changed under
        // view_mutex_.
        FrameGenEngineImageTracker tracker;
        std::optional<std::array<std::uint64_t, kFrameGenEngineImages>> last_end;
        std::uint64_t last_end_frame{0};
        std::uint32_t last_images{0};
        // Each image's hash at its own last census at a frame's end, for the
        // images read so far (bits): stage 2 reads one most frames.
        std::array<std::uint64_t, kFrameGenEngineImages> last_hash{};
        std::uint32_t known{0};
        // The worker thread's alone.
        std::map<std::uint64_t, Hashes> hashes;
        std::optional<AxisSolutions> gain; // the last window's fit that solved
        // This window's.
        std::array<ImageWindow, kFrameGenEngineImages> images{};
        std::uint64_t informative{0};
        std::array<FrameGenEngineAxisFit, 2> fit{};
        std::array<FrameGenEngineAxisFit, 2> lag{};
        std::vector<FrameSamples> frames;
        Comparison early_end;
        Comparison end_next_early;
    };

    void Run();
    void Process(std::deque<Frame>& frames, std::vector<FrameGenEngineCensus>& census);
    void ProcessCensus(const FrameGenEngineCensus& census);
    void Compare(ViewState& state, std::size_t view, std::uint64_t frame);
    void WriteRow(const FrameGenEngineCensus& census, const Frame* record, bool informative, double head_degrees,
                  double head_mm, std::optional<std::uint32_t> written, const ViewState& state,
                  std::optional<std::array<double, 2>> intercept_px);
    // The fit a view's intercepts are measured with: this window's when it
    // solves, else the last window's that did.
    static std::optional<AxisSolutions> Gain(const ViewState& state);
    // Worker thread, or Flush with the thread idle.
    std::string Summary();
    void NewWindow();

    Logger* logger_{nullptr};
    std::chrono::milliseconds summary_interval_;
    // What the game's frames hand over, and the thread's state.
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable idle_;
    bool stop_{false};
    bool busy_{false};
    std::deque<Frame> frames_in_;
    std::vector<FrameGenEngineCensus> census_in_;
    std::uint64_t census_seen_{0}; // the newest sequence taken
    FrameGenEngineStatus counts_;   // the backend's counts (no censuses)
    // The worker thread's from here: the counts as last taken, the frames'
    // records, the views.
    FrameGenEngineStatus counts_taken_;
    std::map<std::uint64_t, Frame> history_; // by projection frame, the newest few
    mutable std::mutex view_mutex_;          // views_' size and what Image and Summarize read
    std::vector<ViewState> views_;
    // This window's records: frames, structures with problems by kind, and
    // nearZ changes between frames; census submissions' GPU time.
    std::uint64_t window_frames_{0};
    std::array<std::uint64_t, 4> window_problems_{};
    std::uint64_t window_near_flips_{0};
    std::optional<float> last_near_;
    double window_gpu_ms_{0.0};
    std::uint64_t window_gpu_samples_{0};
    std::chrono::steady_clock::time_point window_start_{};
    std::string csv_path_;
    std::ofstream csv_;
    std::chrono::steady_clock::time_point csv_start_{};
    bool csv_closed_{false};
    std::thread thread_;
};

} // namespace depthxr
