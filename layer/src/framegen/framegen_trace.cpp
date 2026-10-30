#include <windows.h>

#include "depthxr/framegen_trace.h"

#include "depthxr/config_path.h"
#include "depthxr/logger.h"

#include <algorithm>
#include <cinttypes>
#include <cmath>

namespace depthxr {
using namespace std::chrono_literals;

FrameGenTrace::~FrameGenTrace() {
    enabled_.store(false, std::memory_order_release);
    {
        std::scoped_lock lock(mutex_);
        stopping_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) {
        worker_.detach();
    }
}

void FrameGenTrace::Start(Logger& logger) {
    std::scoped_lock lifecycle_lock(lifecycle_mutex_);
    if (worker_.joinable()) {
        return;
    }
    std::filesystem::path path = logger.ActiveLogPath();
    if (path.empty()) {
        return;
    }
    path.replace_extension(".framegen-trace.csv");
    // Appended: a second session in the process, or debug logging turned on
    // again, writes after what is there.
    std::FILE* file = _wfopen(path.c_str(), L"ab");
    if (!file) {
        logger.Error("Frame Generation trace: could not open " + PathToUtf8(path) + "; no trace this session.");
        return;
    }
    // Times: ns is steady time since the trace started; XrTime fields are the
    // runtime's. Poses: a b c position (um), d e f orientation xyz (1e-6, w >= 0).
    if (std::fseek(file, 0, SEEK_END) == 0 && std::ftell(file) == 0) {
        std::fputs("name,ns,thread,id,a,b,c,d,e,f\n", file);
    }
    std::fputs("session,0,0,0,0,0,0,0,0,0\n", file);
    {
        std::scoped_lock lock(mutex_);
        file_ = file;
        stopping_ = false;
        accepted_ = 0;
        dropped_ = 0;
        queue_.clear();
        queue_.reserve(kCapacity);
        start_ = std::chrono::steady_clock::now();
    }
    try {
        worker_ = std::thread([this] { Run(); });
    } catch (...) {
        std::scoped_lock lock(mutex_);
        std::fclose(file_);
        file_ = nullptr;
        logger.Error("Frame Generation trace: its writer could not start; no trace this session.");
        return;
    }
    enabled_.store(true, std::memory_order_release);
    logger.Info(
        "Frame Generation trace (debug logging): writing " + PathToUtf8(path) +
        " (wait: id=app frame a=predictedDisplayTime b=period c=shouldRender; begin: a=result; "
        "end: a=displayTime b=layerCount c=projectionViews d=view0 swapchain e=view1 swapchain; "
        "end.v0/end.v1: pose; acq/rel/hand (only while frames are generated or about to be): id=swapchain "
        "a=index, b=copied (rel: 2 when a bridge hands it over at the frame's end; hand: that hand-over); "
        "cap: id=swapchain a=index b=captured c=jobs d=releases e=GPU mark; "
        "keep: id=real frame a=time b=previousTime c=analyzed d=previousAnalyzed e=asIs, keep.v0: its pose; "
        "pick: id=frame shown a=refresh time b=its time c=flags d=fraction*1e6 e=previousTime f=chosen before "
        "fallback; ringcap (the lead's ceiling moved for the history ring: lowered, put back by late frames, "
        "or dropped as Dynamic slowed): id=multiplier a=ceiling (0 none) b=lead before; sub: a=displayTime b=layerCount c=view0 swapchain d=view0 image width, sub.v0/sub.v1: pose; "
        "end.q (the game's first quad layer): id=layer index a=space kind b=width um c=height um d=swapchain "
        "e=eyeVisibility, end.q0: its pose (id=space kind); loc (the runtime's answer to the game's stereo "
        "xrLocateViews): a=displayTime b=space kind c=viewCount d=viewStateFlags, loc.v0/loc.v1: pose; space kinds: "
        "0 other, 1 VIEW, 2 LOCAL, 3 STAGE).");
}

void FrameGenTrace::Stop() {
    std::scoped_lock lifecycle_lock(lifecycle_mutex_);
    enabled_.store(false, std::memory_order_release);
    {
        std::scoped_lock lock(mutex_);
        stopping_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
    std::scoped_lock lock(mutex_);
    if (file_) {
        std::fclose(file_);
        file_ = nullptr;
    }
}

void FrameGenTrace::Record(const char* name, std::uint64_t id, std::int64_t a, std::int64_t b, std::int64_t c,
                           std::int64_t d, std::int64_t e, std::int64_t f) {
    if (!Enabled()) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    const DWORD thread = GetCurrentThreadId();
    std::scoped_lock lock(mutex_);
    if (stopping_ || accepted_ >= kMaximumEvents) {
        return;
    }
    if (queue_.size() >= kCapacity) {
        ++dropped_;
        return;
    }
    queue_.push_back(Event{name, std::chrono::duration_cast<std::chrono::nanoseconds>(now - start_).count(),
                           static_cast<std::uint32_t>(thread), id, a, b, c, d, e, f});
    ++accepted_;
    if (queue_.size() == kCapacity / 2) {
        cv_.notify_one();
    }
}

void FrameGenTrace::RecordPose(const char* name, std::uint64_t id, const XrPosef& pose) {
    const XrQuaternionf& q = pose.orientation;
    const float sign = q.w < 0.0f ? -1.0f : 1.0f;
    const auto micro = [](float value) { return static_cast<std::int64_t>(std::llround(value * 1e6)); };
    Record(name, id, micro(pose.position.x), micro(pose.position.y), micro(pose.position.z), micro(q.x * sign),
           micro(q.y * sign), micro(q.z * sign));
}

void FrameGenTrace::Run() {
    std::vector<Event> batch;
    batch.reserve(kCapacity);
    char line[512];
    bool limit_written = false;
    for (;;) {
        std::uint64_t dropped = 0;
        bool stopping = false;
        bool limit = false;
        std::FILE* file = nullptr;
        {
            std::unique_lock lock(mutex_);
            cv_.wait_for(lock, 250ms, [this] { return stopping_ || queue_.size() >= kCapacity / 2; });
            batch.swap(queue_);
            dropped = dropped_;
            dropped_ = 0;
            stopping = stopping_;
            limit = accepted_ >= kMaximumEvents;
            file = file_;
        }
        if (file) {
            for (const Event& event : batch) {
                const int length = std::snprintf(
                    line, sizeof(line),
                    "%s,%" PRId64 ",%" PRIu32 ",%" PRIu64 ",%" PRId64 ",%" PRId64 ",%" PRId64 ",%" PRId64 ",%" PRId64
                    ",%" PRId64 "\n",
                    event.name, event.ns, event.thread, event.id, event.a, event.b, event.c, event.d, event.e,
                    event.f);
                if (length > 0) {
                    std::fwrite(line, 1, static_cast<std::size_t>(std::min<int>(length, sizeof(line) - 1)), file);
                }
            }
            if (dropped) {
                std::fprintf(file, "dropped,0,0,%" PRIu64 ",0,0,0,0,0,0\n", dropped);
            }
            if (limit && !limit_written) {
                std::fputs("limit,0,0,0,0,0,0,0,0,0\n", file);
                limit_written = true;
            }
            std::fflush(file);
        }
        batch.clear();
        if (stopping) {
            break;
        }
    }
}

} // namespace depthxr
