#include "depthxr/engine_motion.h"

#include "depthxr/logger.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace depthxr {
namespace {

// Censuses of a view kept by frame while their early and end readbacks may
// still meet, and frames' records kept for the fits (three in a row) and the
// readbacks' latency.
constexpr std::size_t kKeptHashes = 16;
constexpr std::size_t kKeptFrames = 32;
// Informative frames: the head moved more than this since the frame before,
// or the application's space (appSpaceDeltaPose) more than the last.
constexpr double kInformativeDegrees = 0.3;
constexpr double kInformativeMeters = 0.002;
constexpr double kInformativeDeltaMeters = 0.05;
// The CSV is written for this long from its first row.
constexpr std::chrono::minutes kCsvLength{20};
// The census grid sizes (FG_PASS_ENGINE_CENSUS).
constexpr std::uint64_t kHashTexels = 64 * 64;
constexpr std::size_t kSampleGrid = 16;

std::string Number(double value, int digits) {
    char text[64];
    std::snprintf(text, sizeof(text), "%.*f", digits, value);
    return text;
}

std::string Percent(std::uint64_t part, std::uint64_t whole) {
    return whole ? Number(100.0 * static_cast<double>(part) / static_cast<double>(whole), 1) + "%" : "-";
}

FrameGenViewGeometry Geometry(const XrCompositionLayerProjectionView& view) {
    FrameGenViewGeometry geometry;
    geometry.orientation = {view.pose.orientation.x, view.pose.orientation.y, view.pose.orientation.z,
                            view.pose.orientation.w};
    geometry.position = {view.pose.position.x, view.pose.position.y, view.pose.position.z};
    geometry.angle_left = view.fov.angleLeft;
    geometry.angle_right = view.fov.angleRight;
    geometry.angle_up = view.fov.angleUp;
    geometry.angle_down = view.fov.angleDown;
    return geometry;
}

const XrCompositionLayerDepthInfoKHR* LeadingDepth(const void* next) {
    for (auto* node = static_cast<const XrBaseInStructure*>(next); node; node = node->next) {
        if (node->type == XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR) {
            return reinterpret_cast<const XrCompositionLayerDepthInfoKHR*>(node);
        }
    }
    return nullptr;
}

bool SameSubImage(const XrSwapchainSubImage& a, const XrSwapchainSubImage& b) {
    return a.swapchain == b.swapchain && a.imageArrayIndex == b.imageArrayIndex &&
           a.imageRect.offset.x == b.imageRect.offset.x && a.imageRect.offset.y == b.imageRect.offset.y &&
           a.imageRect.extent.width == b.imageRect.extent.width && a.imageRect.extent.height == b.imageRect.extent.height;
}

// How far the head turned (degrees) and moved (meters) between two poses.
std::pair<double, double> Step(const FrameGenViewGeometry& a, const FrameGenViewGeometry& b) {
    double dot = 0.0;
    for (int i = 0; i < 4; ++i) dot += static_cast<double>(a.orientation[i]) * b.orientation[i];
    const double degrees = 2.0 * std::acos(std::min(1.0, std::abs(dot))) * 57.29577951308232;
    double squared = 0.0;
    for (int i = 0; i < 3; ++i) {
        const double d = static_cast<double>(a.position[i]) - b.position[i];
        squared += d * d;
    }
    return {degrees, std::sqrt(squared)};
}

double Length(const XrVector3f& v) {
    return std::sqrt(static_cast<double>(v.x) * v.x + static_cast<double>(v.y) * v.y + static_cast<double>(v.z) * v.z);
}

// Where a view is in a record: its per-view vectors are in the record's
// order, which need not be the views' own.
std::optional<std::size_t> Place(const FrameGenEngineMotionFrame& record, std::size_t view) {
    for (std::size_t i = 0; i < record.views.size(); ++i) {
        if (record.views[i].view == view) return i;
    }
    return std::nullopt;
}

std::string Hex(std::uint64_t value) {
    char text[32];
    std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(value));
    return text;
}

} // namespace

const char* ToString(FrameGenMotionSource source) {
    switch (source) {
    case FrameGenMotionSource::kSpaceWarpFb:
        return "fb";
    case FrameGenMotionSource::kFrameSynthesisExt:
        return "ext";
    default:
        return "none";
    }
}

bool IsEngineMotionFormat(std::int64_t format) {
    constexpr std::int64_t kFormats[] = {
        10, 34, 2,              // DXGI R16G16B16A16_FLOAT, R16G16_FLOAT, R32G32B32A32_FLOAT
        97, 83, 109,            // VK_FORMAT_R16G16B16A16_SFLOAT, R16G16_SFLOAT, R32G32B32A32_SFLOAT
        0x881A, 0x822F, 0x8814, // GL_RGBA16F, GL_RG16F, GL_RGBA32F
    };
    return std::find(std::begin(kFormats), std::end(kFormats), format) != std::end(kFormats);
}

std::uint32_t EngineMotionProblems(const SpaceWarpViewInfo& view, const std::optional<EngineMotionSwapchain>& vectors) {
    std::uint32_t problems = 0;
    if (view.near_z == view.far_z) {
        problems |= kEngineMotionNearFar;
    }
    if (!(view.min_depth >= 0.0f && view.min_depth <= view.max_depth && view.max_depth <= 1.0f)) {
        problems |= kEngineMotionDepthRange;
    }
    const XrRect2Di& rect = view.motion_vector.imageRect;
    if (rect.offset.x < 0 || rect.offset.y < 0 || rect.extent.width <= 0 || rect.extent.height <= 0) {
        problems |= kEngineMotionRect;
    }
    if (vectors) {
        if (static_cast<std::int64_t>(rect.offset.x) + rect.extent.width > vectors->width ||
            static_cast<std::int64_t>(rect.offset.y) + rect.extent.height > vectors->height ||
            view.motion_vector.imageArrayIndex >= std::max(vectors->array_size, 1u)) {
            problems |= kEngineMotionRect;
        }
        if (!IsEngineMotionFormat(vectors->format)) {
            problems |= kEngineMotionFormat;
        }
    }
    return problems;
}

bool ValidateEngineMotionView(const SpaceWarpViewInfo& view, std::string& why,
                              const std::optional<EngineMotionSwapchain>& vectors) {
    const std::uint32_t problems = EngineMotionProblems(view, vectors);
    why.clear();
    auto add = [&](const char* text) { why += (why.empty() ? "" : "; ") + std::string(text); };
    if (problems & kEngineMotionNearFar) add("nearZ equals farZ");
    if (problems & kEngineMotionDepthRange) add("minDepth and maxDepth are not 0 <= minDepth <= maxDepth <= 1");
    if (problems & kEngineMotionRect) add("the motion vector region is empty or outside its swapchain");
    if (problems & kEngineMotionFormat) add("the motion vector swapchain's format is not RGBA16F, RG16F or RGBA32F");
    return problems == 0;
}

std::optional<FrameGenEngineMotionFrame> ToEngineMotionFrame(const SpaceWarpFrame& storage, const XrFrameEndInfo& stripped,
                                                             std::uint64_t projection_frame,
                                                             const EngineMotionSwapchains& swapchains) {
    // The primary projection: the first projection layer with views, as
    // Frame Generation finds it.
    const XrCompositionLayerProjection* projection = nullptr;
    std::uint32_t layer = 0;
    for (std::uint32_t i = 0; i < stripped.layerCount && !projection; ++i) {
        const XrCompositionLayerBaseHeader* base = stripped.layers ? stripped.layers[i] : nullptr;
        if (base && base->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
            const auto* candidate = reinterpret_cast<const XrCompositionLayerProjection*>(base);
            if (candidate->viewCount > 0 && candidate->views) {
                projection = candidate;
                layer = i;
            }
        }
    }
    if (!projection) {
        return std::nullopt;
    }
    FrameGenEngineMotionFrame frame;
    frame.display_time = stripped.displayTime;
    frame.projection_frame = projection_frame;
    frame.primary_layer = layer;
    bool one_source = true;
    for (const SpaceWarpViewInfo& found : storage.found) {
        // Only structures taken off the primary projection's own views: one
        // left behind a structure the probe cannot copy went to the runtime.
        if (!found.stripped || found.layer_index != layer || found.view_index >= projection->viewCount) {
            continue;
        }
        const XrCompositionLayerProjectionView& view = projection->views[found.view_index];
        FrameGenEngineMotionView motion;
        motion.view = found.view_index;
        const bool fb = found.type == XR_TYPE_COMPOSITION_LAYER_SPACE_WARP_INFO_FB;
        motion.source = fb ? FrameGenMotionSource::kSpaceWarpFb : FrameGenMotionSource::kFrameSynthesisExt;
        motion.vectors = found.motion_vector;
        motion.depth = found.depth;
        motion.color = view.subImage;
        motion.depth_encoding = {found.min_depth, found.max_depth, found.near_z, found.far_z};
        const XrCompositionLayerDepthInfoKHR* depth = LeadingDepth(view.next);
        motion.depth_is_khr = depth && SameSubImage(depth->subImage, found.depth);
        if (!fb) {
            motion.encoding.ext = true;
            motion.encoding.scale = {found.motion_vector_scale.x, found.motion_vector_scale.y,
                                     found.motion_vector_scale.z};
            motion.encoding.offset = {found.motion_vector_offset.x, found.motion_vector_offset.y,
                                      found.motion_vector_offset.z};
            motion.encoding.two_d = (found.layer_flags & XR_FRAME_SYNTHESIS_INFO_USE_2D_MOTION_VECTOR_BIT_EXT) != 0;
            motion.relaxed_interval =
                (found.layer_flags & XR_FRAME_SYNTHESIS_INFO_REQUEST_RELAXED_FRAME_INTERVAL_BIT_EXT) != 0;
        } else {
            motion.frame_skip = (found.layer_flags & XR_COMPOSITION_LAYER_SPACE_WARP_INFO_FRAME_SKIP_BIT_FB) != 0;
        }
        motion.layer_flags = found.layer_flags;
        motion.app_delta = found.app_space_delta_pose;
        // The view as the game submitted it: the layer's own changes come
        // later, in a copy.
        motion.game = Geometry(view);
        const std::optional<EngineMotionSwapchain> vectors =
            swapchains ? swapchains(found.motion_vector.swapchain) : std::nullopt;
        motion.vectors_format = vectors ? vectors->format : 0;
        motion.problems = EngineMotionProblems(found, vectors);
        frame.invalid += motion.problems ? 1u : 0u;
        one_source = one_source && (frame.views.empty() || frame.views.front().source == motion.source);
        frame.views.push_back(motion);
    }
    if (frame.views.empty()) {
        return std::nullopt;
    }
    std::vector<bool> seen(projection->viewCount, false);
    for (const FrameGenEngineMotionView& view : frame.views) {
        seen[view.view] = true;
    }
    frame.complete = one_source && frame.views.size() == projection->viewCount &&
                     std::all_of(seen.begin(), seen.end(), [](bool value) { return value; });
    return frame;
}

FrameGenEngineCensusWorker::FrameGenEngineCensusWorker(Logger* logger, std::string csv_path,
                                                       std::chrono::milliseconds summary_interval)
    : logger_(logger), summary_interval_(summary_interval), csv_path_(std::move(csv_path)) {
    window_start_ = std::chrono::steady_clock::now();
    thread_ = std::thread([this] { Run(); });
}

FrameGenEngineCensusWorker::~FrameGenEngineCensusWorker() {
    {
        std::scoped_lock lock(mutex_);
        stop_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
}

void FrameGenEngineCensusWorker::Abandon() {
    {
        std::scoped_lock lock(mutex_);
        stop_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable()) {
        thread_.detach();
    }
}

void FrameGenEngineCensusWorker::NoteFrame(Frame frame) {
    {
        std::scoped_lock lock(mutex_);
        frames_in_.push_back(std::move(frame));
    }
    wake_.notify_all();
}

void FrameGenEngineCensusWorker::NoteStatus(const FrameGenEngineStatus& status) {
    {
        std::scoped_lock lock(mutex_);
        counts_ = status;
        counts_.census.clear();
        counts_.notes.clear();
        for (const FrameGenEngineCensus& census : status.census) {
            if (census.sequence > census_seen_) {
                census_seen_ = census.sequence;
                census_in_.push_back(census);
            }
        }
    }
    wake_.notify_all();
}

std::optional<std::uint32_t> FrameGenEngineCensusWorker::Image(std::size_t view, std::uint64_t frame,
                                                               std::uint32_t color_index, std::uint32_t depth_index,
                                                               std::optional<std::uint32_t> released) const {
    std::scoped_lock lock(view_mutex_);
    if (view >= views_.size()) {
        return released;
    }
    return views_[view].tracker.Image(frame, color_index, depth_index, released);
}

std::optional<FrameGenEngineCensusWorker::ViewSummary> FrameGenEngineCensusWorker::Summarize(std::size_t view) const {
    std::scoped_lock lock(view_mutex_);
    if (view >= views_.size()) {
        return std::nullopt;
    }
    const ViewState& state = views_[view];
    ViewSummary summary;
    summary.best = state.tracker.Best();
    summary.locked = state.tracker.Locked();
    summary.census_frame = state.last_end_frame;
    summary.images = state.last_images;
    if (state.last_end) summary.hashes = *state.last_end;
    return summary;
}

std::string FrameGenEngineCensusWorker::Flush() {
    std::unique_lock lock(mutex_);
    wake_.notify_all();
    idle_.wait(lock, [&] { return stop_ || (!busy_ && frames_in_.empty() && census_in_.empty()); });
    // The thread is idle, and stays so while the lock is held.
    counts_taken_ = counts_;
    return Summary();
}

void FrameGenEngineCensusWorker::Run() {
    std::unique_lock lock(mutex_);
    while (!stop_) {
        wake_.wait_for(lock, std::chrono::milliseconds(250),
                       [&] { return stop_ || !frames_in_.empty() || !census_in_.empty(); });
        if (stop_) {
            break;
        }
        std::deque<Frame> frames = std::move(frames_in_);
        frames_in_.clear();
        std::vector<FrameGenEngineCensus> census = std::move(census_in_);
        census_in_.clear();
        counts_taken_ = counts_;
        busy_ = true;
        // The work, the CSV and the log without the lock the game's frames
        // take to hand theirs over.
        lock.unlock();
        Process(frames, census);
        if (std::chrono::steady_clock::now() - window_start_ >= summary_interval_ && window_frames_ > 0) {
            const std::string line = Summary();
            NewWindow();
            if (logger_) {
                logger_->Info(line);
            }
        }
        lock.lock();
        busy_ = false;
        idle_.notify_all();
    }
    idle_.notify_all();
}

void FrameGenEngineCensusWorker::Process(std::deque<Frame>& frames, std::vector<FrameGenEngineCensus>& census) {
    for (Frame& frame : frames) {
        ++window_frames_;
        for (const FrameGenEngineMotionView& view : frame.record.views) {
            for (std::size_t bit = 0; bit < window_problems_.size(); ++bit) {
                window_problems_[bit] += (view.problems & (1u << bit)) ? 1u : 0u;
            }
        }
        if (!frame.record.views.empty()) {
            const float near_z = frame.record.views.front().depth_encoding.near_z;
            if (last_near_ && *last_near_ != near_z) {
                ++window_near_flips_;
            }
            last_near_ = near_z;
        }
        if (frame.record.views.empty()) {
            continue; // nothing a census pairs with (and never in place of a frame's that has)
        }
        const std::uint64_t key = frame.record.projection_frame;
        history_[key] = std::move(frame);
        while (history_.size() > kKeptFrames) {
            history_.erase(history_.begin());
        }
    }
    // Readbacks arrive in the order they were read, which is the order they
    // were submitted in.
    std::sort(census.begin(), census.end(),
              [](const FrameGenEngineCensus& a, const FrameGenEngineCensus& b) { return a.sequence < b.sequence; });
    for (const FrameGenEngineCensus& entry : census) {
        ProcessCensus(entry);
    }
}

void FrameGenEngineCensusWorker::ProcessCensus(const FrameGenEngineCensus& census) {
    if (views_.size() <= census.view) {
        std::scoped_lock lock(view_mutex_);
        views_.resize(census.view + 1);
    }
    ViewState& state = views_[census.view];
    if (census.gpu_ms >= 0.0) {
        window_gpu_ms_ += census.gpu_ms;
        ++window_gpu_samples_;
    }
    const std::uint32_t images = std::min(census.images, kFrameGenEngineImages);
    std::array<std::uint64_t, kFrameGenEngineImages> hashes{};
    for (std::uint32_t i = 0; i < images; ++i) {
        hashes[i] = census.image[i].Hash();
    }
    Hashes& entry = state.hashes[census.frame];
    if (census.early) {
        entry.early = hashes;
        Compare(state, census.view, census.frame);
        Compare(state, census.view, census.frame + 1);
        while (state.hashes.size() > kKeptHashes) state.hashes.erase(state.hashes.begin());
        return;
    }
    // The images that changed since this view's last census.
    std::uint32_t changed = 0;
    std::uint32_t changed_count = 0;
    std::optional<std::uint32_t> written;
    if (state.last_end) {
        for (std::uint32_t i = 0; i < images; ++i) {
            if ((*state.last_end)[i] != hashes[i]) {
                changed |= 1u << i;
                ++changed_count;
                written = i;
            }
        }
        if (changed_count != 1) written.reset();
    }
    {
        std::scoped_lock lock(view_mutex_);
        state.last_end = hashes;
        state.last_end_frame = census.frame;
        state.last_images = images;
    }
    entry.end = hashes;
    entry.written = written;
    Compare(state, census.view, census.frame);
    Compare(state, census.view, census.frame + 1);
    while (state.hashes.size() > kKeptHashes) state.hashes.erase(state.hashes.begin());

    // The frame's record, and the two before it while continuous.
    const auto found = history_.find(census.frame);
    const Frame* record = found != history_.end() ? &found->second : nullptr;
    const Frame* previous = nullptr;
    const Frame* before_previous = nullptr;
    if (record && record->continuous) {
        const auto it = history_.find(census.frame - 1);
        previous = it != history_.end() ? &it->second : nullptr;
        if (previous && previous->continuous) {
            const auto earlier = history_.find(census.frame - 2);
            before_previous = earlier != history_.end() ? &earlier->second : nullptr;
        }
    }
    auto view_of = [&](const Frame* frame) -> const FrameGenEngineMotionView* {
        if (!frame) return nullptr;
        const std::optional<std::size_t> place = Place(frame->record, census.view);
        return place ? &frame->record.views[*place] : nullptr;
    };
    const std::optional<std::size_t> place = record ? Place(record->record, census.view) : std::nullopt;
    const FrameGenEngineMotionView* newest = view_of(record);
    const FrameGenEngineMotionView* older = view_of(previous);
    const FrameGenEngineMotionView* oldest = view_of(before_previous);
    bool informative = false;
    double head_degrees = 0.0;
    double head_mm = 0.0;
    if (newest && older) {
        const auto [degrees, meters] = Step(newest->game, older->game);
        head_degrees = degrees;
        head_mm = 1000.0 * meters;
        informative = degrees > kInformativeDegrees || meters > kInformativeMeters ||
                      Length(newest->app_delta.position) > kInformativeDeltaMeters;
    }

    // Which image is written.
    FrameGenEngineTrackerFrame tracked;
    tracked.frame = census.frame;
    tracked.informative = informative;
    tracked.handle = census.handle;
    tracked.rect = census.rect;
    tracked.images = images;
    tracked.hashes = hashes;
    tracked.color_index = census.color_index;
    tracked.depth_index = census.depth_index;
    if (place && *place < record->released.size()) {
        tracked.released = record->released[*place];
    }
    {
        std::scoped_lock lock(view_mutex_);
        state.tracker.Add(tracked);
    }
    if (informative) {
        ++state.informative;
        for (std::uint32_t i = 0; i < images; ++i) {
            state.images[i].changed += (changed >> i) & 1u;
        }
    }
    for (std::uint32_t i = 0; i < images; ++i) {
        state.images[i].zeros += census.image[i].zeros;
        state.images[i].non_finite += census.image[i].non_finite;
        state.images[i].texels += kHashTexels;
    }

    // The online fit: cockpit samples of the image written (the one that
    // changed, else the tracker's), against the head's movement between the
    // frame and the one before, and between the two before (lag). The
    // frame's samples are kept summed for its intercept under the window's
    // fit: with the head still, the vectors' mean (a fit of the frame alone
    // would follow tracking noise).
    std::optional<std::array<double, 2>> intercept_px;
    const std::optional<std::uint32_t> image =
        written ? written
                : state.tracker.Image(census.frame, census.color_index, census.depth_index, tracked.released);
    if (newest && older && image && *image < images && census.samples.size() >= kSampleGrid * kSampleGrid * 4 &&
        *place < record->color_extent.size()) {
        const FrameGenExtent half = FrameGenPyramid(record->color_extent[*place])[1];
        FrameSamples sums;
        sums.pixels = {0.5 * std::max(census.rect[2], 1), 0.5 * std::max(census.rect[3], 1)};
        std::array<FrameGenEngineAxisFit, 2>& frame_fit = sums.fit;
        for (std::size_t s = 0; s < kSampleGrid * kSampleGrid; ++s) {
            const std::array<float, 4>& raw = census.samples[s * 4 + *image];
            const std::array<float, 4>& where = census.samples[s * 4 + 3];
            const float depth = where[0];
            const std::array<float, 2> e = DecodeFrameGenMotion(newest->encoding, raw);
            if (!(depth > 0.0f) || depth >= kFrameGenEngineCockpit || !std::isfinite(e[0]) || !std::isfinite(e[1]) ||
                half.width == 0 || half.height == 0) {
                continue;
            }
            const std::array<float, 2> uv{(where[2] + 0.5f) / static_cast<float>(half.width),
                                          (where[3] + 0.5f) / static_cast<float>(half.height)};
            const std::array<float, 4> delta{newest->app_delta.orientation.x, newest->app_delta.orientation.y,
                                             newest->app_delta.orientation.z, newest->app_delta.orientation.w};
            const std::array<float, 3> delta_position{newest->app_delta.position.x, newest->app_delta.position.y,
                                                      newest->app_delta.position.z};
            auto add = [&](const FrameGenViewGeometry& to, const FrameGenViewGeometry& from,
                           std::array<FrameGenEngineAxisFit, 2>& fit, std::array<FrameGenEngineAxisFit, 2>* each) {
                std::array<double, 2> rotation{};
                std::array<double, 2> full{};
                if (!FrameGenEnginePrediction(to, from, delta, delta_position, 1, FrameGenEngineCamera::kRotationOnly, uv,
                                              depth, rotation) ||
                    !FrameGenEnginePrediction(to, from, delta, delta_position, 1, FrameGenEngineCamera::kIncluded, uv,
                                              depth, full)) {
                    return;
                }
                for (int axis = 0; axis < 2; ++axis) {
                    fit[axis].Add(e[axis], rotation[axis], full[axis] - rotation[axis]);
                    if (each) (*each)[axis].Add(e[axis], rotation[axis], full[axis] - rotation[axis]);
                }
            };
            add(newest->game, older->game, state.fit, &frame_fit);
            if (oldest) {
                add(older->game, oldest->game, state.lag, nullptr);
            }
        }
        if (frame_fit[0].Samples() > 0.0) {
            // In vector pixels once a fit gives the gain: c / a is in NDC.
            if (const std::optional<AxisSolutions> gain = Gain(state)) {
                intercept_px = {frame_fit[0].Intercept((*gain)[0].a, (*gain)[0].b) / (*gain)[0].a * sums.pixels[0],
                                frame_fit[1].Intercept((*gain)[1].a, (*gain)[1].b) / (*gain)[1].a * sums.pixels[1]};
            }
            state.frames.push_back(sums);
        }
    }
    WriteRow(census, record, informative, head_degrees, head_mm, written, state, intercept_px);
}

void FrameGenEngineCensusWorker::Compare(ViewState& state, std::size_t view, std::uint64_t frame) {
    const auto it = state.hashes.find(frame);
    if (it == state.hashes.end()) {
        return;
    }
    Hashes& current = it->second;
    // The image written at the frame's end was complete when its depth was
    // released.
    if (current.end && current.early && current.written && !current.compared_early) {
        current.compared_early = true;
        ++state.early_end.compared;
        state.early_end.equal += (*current.early)[*current.written] == (*current.end)[*current.written] ? 1u : 0u;
    }
    // ...and the frame before's was not written again after its xrEndFrame:
    // unchanged by this frame's depth release, unless this frame writes it.
    const auto before = state.hashes.find(frame - 1);
    if (before == state.hashes.end() || !before->second.end || !before->second.written || !current.early ||
        before->second.compared_next) {
        return;
    }
    const std::uint32_t image = *before->second.written;
    std::optional<std::uint32_t> rewritten = current.written;
    if (!current.end) {
        const auto record = history_.find(frame);
        const std::optional<std::size_t> place =
            record != history_.end() ? Place(record->second.record, view) : std::nullopt;
        if (!place || *place >= record->second.color_index.size() || *place >= record->second.depth_index.size()) {
            return; // wait for this frame's end
        }
        rewritten = state.tracker.Image(frame, record->second.color_index[*place], record->second.depth_index[*place],
                                        *place < record->second.released.size() ? record->second.released[*place]
                                                                                : std::nullopt);
    }
    before->second.compared_next = true;
    if (rewritten && *rewritten == image) {
        return;
    }
    ++state.end_next_early.compared;
    state.end_next_early.equal += (*before->second.end)[image] == (*current.early)[image] ? 1u : 0u;
}

void FrameGenEngineCensusWorker::WriteRow(const FrameGenEngineCensus& census, const Frame* record, bool informative,
                                          double head_degrees, double head_mm, std::optional<std::uint32_t> written,
                                          const ViewState& state, std::optional<std::array<double, 2>> intercept_px) {
    if (csv_path_.empty() || csv_closed_) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (!csv_.is_open()) {
        csv_.open(csv_path_, std::ios::out | std::ios::trunc);
        if (!csv_) {
            csv_closed_ = true;
            return;
        }
        csv_start_ = now;
        csv_ << "time_s,frame,view,display_time,informative,head_deg,head_mm,delta_m,images,written,policy,"
                "policy_share,locked";
        for (std::uint32_t i = 0; i < kFrameGenEngineImages; ++i) {
            csv_ << ",hash" << i << ",zero" << i << ",nonfinite" << i << ",mean_r" << i << ",mean_g" << i << ",mean_abs"
                 << i << ",max_abs" << i << ",large" << i;
        }
        csv_ << ",intercept_x_px,intercept_y_px,near_z,far_z,min_depth,max_depth,problems,gpu_ms\n";
    }
    if (now - csv_start_ > kCsvLength) {
        csv_.close();
        csv_closed_ = true;
        if (logger_) {
            logger_->Info("Frame Generation engine motion census: the CSV stops after its first 20 minutes (" + csv_path_ +
                          ").");
        }
        return;
    }
    const FrameGenEngineMotionView* view = nullptr;
    if (record) {
        for (const FrameGenEngineMotionView& candidate : record->record.views) {
            if (candidate.view == census.view) view = &candidate;
        }
    }
    const FrameGenEngineImageTracker::Hypothesis best = state.tracker.Best();
    csv_ << Number(std::chrono::duration<double>(now - csv_start_).count(), 3) << ',' << census.frame << ','
         << census.view << ',' << (record ? record->record.display_time : 0) << ',' << (informative ? 1 : 0) << ','
         << Number(head_degrees, 4) << ',' << Number(head_mm, 3) << ','
         << Number(view ? Length(view->app_delta.position) : 0.0, 4) << ',' << census.images << ','
         << (written ? static_cast<int>(*written) : -1) << ',' << ToString(best.policy) << ' ' << best.parameter << ','
         << Number(best.share, 3) << ',' << (state.tracker.Locked() ? 1 : 0);
    for (std::uint32_t i = 0; i < kFrameGenEngineImages; ++i) {
        const FrameGenEngineImageCensus& image = census.image[i];
        const double finite = std::max<std::uint32_t>(image.finite, 1);
        csv_ << ',' << (i < census.images ? Hex(image.Hash()) : std::string()) << ',' << image.zeros << ','
             << image.non_finite << ',' << Number(image.sums[0] / finite, 6) << ',' << Number(image.sums[1] / finite, 6)
             << ',' << Number(image.sum_abs / finite, 6) << ',' << Number(image.max_abs, 6) << ',' << image.large;
    }
    csv_ << ',' << (intercept_px ? Number((*intercept_px)[0], 4) : std::string()) << ','
         << (intercept_px ? Number((*intercept_px)[1], 4) : std::string()) << ','
         << (view ? Number(view->depth_encoding.near_z, 4) : std::string()) << ','
         << (view ? Number(view->depth_encoding.far_z, 4) : std::string()) << ','
         << (view ? Number(view->depth_encoding.min_depth, 4) : std::string()) << ','
         << (view ? Number(view->depth_encoding.max_depth, 4) : std::string()) << ','
         << (view ? view->problems : 0u) << ',' << (census.gpu_ms >= 0.0 ? Number(census.gpu_ms, 4) : std::string())
         << '\n';
}

std::optional<FrameGenEngineCensusWorker::AxisSolutions> FrameGenEngineCensusWorker::Gain(const ViewState& state) {
    const AxisSolutions fit{state.fit[0].Solve(), state.fit[1].Solve()};
    if (GivesFrameGenEngineGain(fit[0]) && GivesFrameGenEngineGain(fit[1])) {
        return fit;
    }
    return state.gain;
}

std::string FrameGenEngineCensusWorker::Summary() {
    std::string line = "Frame Generation engine motion census (" +
                       Number(std::chrono::duration<double>(std::chrono::steady_clock::now() - window_start_).count(), 0) +
                       " s): frames=" + std::to_string(window_frames_);
    for (std::size_t v = 0; v < views_.size(); ++v) {
        const ViewState& state = views_[v];
        const FrameGenEngineImageTracker& tracker = state.tracker;
        line += "; view" + std::to_string(v) + ": informative=" + std::to_string(state.informative) + ", changed";
        for (std::uint32_t i = 0; i < kFrameGenEngineImages; ++i) {
            line += (i ? "/" : " ") + Percent(state.images[i].changed, state.informative);
        }
        line += ", zero";
        for (std::uint32_t i = 0; i < kFrameGenEngineImages; ++i) {
            line += (i ? "/" : " ") + Percent(state.images[i].zeros, state.images[i].texels);
        }
        line += ", nonFinite";
        for (std::uint32_t i = 0; i < kFrameGenEngineImages; ++i) {
            line += (i ? "/" : " ") + std::to_string(state.images[i].non_finite);
        }
        const FrameGenEngineImageTracker::Hypothesis best = tracker.Best();
        const FrameGenEngineImageTracker::Hypothesis runner_up = tracker.RunnerUp();
        line += ", image=" + std::string(ToString(best.policy)) +
                (best.policy == FrameGenEngineImageTracker::Policy::kConstant ||
                         best.policy == FrameGenEngineImageTracker::Policy::kCycle
                     ? " " + std::to_string(best.parameter)
                     : std::string()) +
                " (" + Number(100.0 * best.share, 1) + "%, " + (tracker.Locked() ? "locked" : "probing") +
                ", runner-up " + ToString(runner_up.policy) + " " + Number(100.0 * runner_up.share, 1) +
                "%; window: single " + std::to_string(tracker.SingleChanges()) + ", all " +
                std::to_string(tracker.AllChanged()) + ", several " + std::to_string(tracker.SeveralChanged()) +
                ", none " + std::to_string(tracker.NoneChanged()) + ")";
        const FrameGenEngineAxisFit::Solution x = state.fit[0].Solve();
        const FrameGenEngineAxisFit::Solution y = state.fit[1].Solve();
        if (x.valid && y.valid) {
            line += ", fit a=(" + Number(x.a, 4) + ", " + Number(y.a, 4) + ") gain=(" +
                    (x.a != 0.0 ? Number(1.0 / x.a, 5) : std::string("-")) + ", " +
                    (y.a != 0.0 ? Number(1.0 / y.a, 5) : std::string("-")) + ") b/a=(" +
                    (x.a != 0.0 ? Number(x.b / x.a, 3) : std::string("-")) + ", " +
                    (y.a != 0.0 ? Number(y.b / y.a, 3) : std::string("-")) + ") corr2=(" + Number(x.corr2, 3) + ", " +
                    Number(y.corr2, 3) + ") n=" + Number(x.samples, 0);
            const FrameGenEngineAxisFit::Solution lag_x = state.lag[0].Solve();
            const FrameGenEngineAxisFit::Solution lag_y = state.lag[1].Solve();
            if (lag_x.valid && lag_y.valid) {
                const double now = 0.5 * (x.corr2 + y.corr2);
                const double lagged = 0.5 * (lag_x.corr2 + lag_y.corr2);
                line += ", lag=" + std::string(now >= lagged ? "0" : "1") + " (corr2 " + Number(now, 3) + " vs " +
                        Number(lagged, 3) + ")";
            }
        } else {
            line += ", fit none (" + Number(state.fit[0].Samples(), 0) + " cockpit samples, too little head movement)";
        }
        // Each frame's intercept under the fit (in vector pixels), or without
        // one yet the frames' mean vectors (in the vectors' own units).
        const std::optional<AxisSolutions> gain = Gain(state);
        std::array<std::vector<double>, 2> intercepts;
        for (const FrameSamples& frame : state.frames) {
            for (int axis = 0; axis < 2; ++axis) {
                intercepts[axis].push_back(gain ? frame.fit[axis].Intercept((*gain)[axis].a, (*gain)[axis].b) /
                                                      (*gain)[axis].a * frame.pixels[axis]
                                                : frame.fit[axis].Intercept(0.0, 0.0));
            }
        }
        auto spread = [](const std::vector<double>& values) {
            if (values.size() < 2) return 0.0;
            double mean = 0.0;
            for (const double value : values) mean += value;
            mean /= static_cast<double>(values.size());
            double variance = 0.0;
            for (const double value : values) variance += (value - mean) * (value - mean);
            return std::sqrt(variance / static_cast<double>(values.size() - 1));
        };
        line += ", jitter=(" + Number(spread(intercepts[0]), 3) + ", " + Number(spread(intercepts[1]), 3) +
                (gain ? ") px" : ") in vector units (no gain fitted yet)") +
                ", earlyEnd=" + std::to_string(state.early_end.equal) + "/" + std::to_string(state.early_end.compared) +
                ", endNextEarly=" + std::to_string(state.end_next_early.equal) + "/" +
                std::to_string(state.end_next_early.compared);
    }
    line += "; structures with problems: nearZ==farZ " + std::to_string(window_problems_[0]) + ", depth range " +
            std::to_string(window_problems_[1]) + ", region " + std::to_string(window_problems_[2]) + ", format " +
            std::to_string(window_problems_[3]) + "; nearZ changes=" + std::to_string(window_near_flips_) +
            "; backend: submitted=" + std::to_string(counts_taken_.submitted) +
            ", busy=" + std::to_string(counts_taken_.busy) + ", refused=" + std::to_string(counts_taken_.refused) +
            ", failed=" + std::to_string(counts_taken_.failed) + ", dumped=" + std::to_string(counts_taken_.dumped) +
            (counts_taken_.stopped ? ", stopped (device removed)" : "") +
            ", censusGpuMs=" +
            (window_gpu_samples_ ? Number(window_gpu_ms_ / static_cast<double>(window_gpu_samples_), 4)
                                 : std::string("-")) +
            ".";
    return line;
}

void FrameGenEngineCensusWorker::NewWindow() {
    window_start_ = std::chrono::steady_clock::now();
    window_frames_ = 0;
    window_problems_ = {};
    window_near_flips_ = 0;
    window_gpu_ms_ = 0.0;
    window_gpu_samples_ = 0;
    for (ViewState& state : views_) {
        // A window with the head still measures its jitter with this gain.
        state.gain = Gain(state);
        state.images = {};
        state.informative = 0;
        state.fit = {};
        state.lag = {};
        state.frames.clear();
        state.early_end = {};
        state.end_next_early = {};
    }
}

} // namespace depthxr
