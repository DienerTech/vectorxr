#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <thread>

namespace depthxr {

// OpenXR Toolkit's Turbo above VectorXR (its layer.cpp xrEndFrame): xrWaitFrame
// on a pool thread as soon as the app's xrEndFrame returns, xrBeginFrame right
// before the next xrEndFrame. Games that wait on a thread of their own begin
// before they render (DCS, MSFS), so all three together are the layer's.
//
// Neither the OS nor the Toolkit promises the short intervals: a pool thread
// can start late, and a begin can be preempted before its end. A frame that
// misses one is not evidence against the pattern, so the frames are judged
// over a window: enough of the last kWindow frames in the pattern is a layer
// pipelining them. With the Toolkit known to be above (its manifest's place in
// the chain) seven in ten of them are; with no such layer known, nearly all.
// Once seen it stays seen until Reset (the session's end).
//
// The times come from the caller (steady_clock in the layer, a script in its
// tests); the caller serializes the calls.
class UpstreamTurboDetector {
  public:
    using Clock = std::chrono::steady_clock;
    static constexpr std::size_t kWindow = 60;
    static constexpr std::size_t kNeededWithToolkitAbove = 42;
    static constexpr std::size_t kNeededOtherwise = 54;
    static constexpr auto kWaitAfterEnd = std::chrono::milliseconds(4);
    static constexpr auto kBeginBeforeEnd = std::chrono::milliseconds(1);

    void Reset() { *this = {}; }
    void Wait(std::thread::id thread, Clock::time_point at) {
        wait_thread_ = thread;
        wait_at_ = at;
    }
    void Begin(Clock::time_point at) { begin_at_ = at; }
    void EndReturned(Clock::time_point at) { end_returned_ = at; }

    // At the app's xrEndFrame on `thread`: the frame is judged, and the
    // answer is whether a layer above pipelines the frames.
    bool End(std::thread::id thread, Clock::time_point at, bool toolkit_above) {
        const bool pattern = wait_thread_ != std::thread::id{} && wait_thread_ != thread &&
                             end_returned_ != Clock::time_point{} && wait_at_ >= end_returned_ &&
                             wait_at_ - end_returned_ < kWaitAfterEnd && begin_at_ > wait_at_ && at >= begin_at_ &&
                             at - begin_at_ < kBeginBeforeEnd;
        if (frames_ == kWindow) {
            matching_ -= window_[next_] ? 1 : 0;
        } else {
            ++frames_;
        }
        window_[next_] = pattern;
        matching_ += pattern ? 1 : 0;
        next_ = (next_ + 1) % kWindow;
        if (!seen_ && frames_ == kWindow &&
            matching_ >= (toolkit_above ? kNeededWithToolkitAbove : kNeededOtherwise)) {
            seen_ = true;
        }
        return seen_;
    }
    bool Seen() const { return seen_; }
    // Of the last frames judged (up to kWindow), how many were in the pattern.
    std::size_t Matching() const { return matching_; }
    std::size_t Frames() const { return frames_; }

  private:
    std::thread::id wait_thread_{};
    Clock::time_point wait_at_{};
    Clock::time_point begin_at_{};
    Clock::time_point end_returned_{};
    std::array<bool, kWindow> window_{};
    std::size_t next_{0};
    std::size_t frames_{0};
    std::size_t matching_{0};
    bool seen_{false};
};

} // namespace depthxr
