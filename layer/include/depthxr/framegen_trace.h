#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <openxr/openxr.h>

namespace depthxr {

class Logger;

// A frame-by-frame record of Frame Generation for one session, for finding
// single frames that step out of the flow (a whole session's 30 s summaries
// average them away). It runs while debug logging is on (the layer starts and
// stops it with the log level). Producers copy fixed numeric records into a
// bounded queue; one thread of its own formats and writes them, beside the
// session's log, as CSV.
class FrameGenTrace {
  public:
    // At process exit (the layer's DLL unloading) the writer is let go, not
    // joined: sessions end through Stop().
    ~FrameGenTrace();

    // Opens <log stem>.framegen-trace.csv beside the active log (appending:
    // debug logging turned off and on again continues the same file).
    void Start(Logger& logger);
    void Stop();
    bool Enabled() const { return enabled_.load(std::memory_order_relaxed); }

    // `name` must be a string literal: it is kept, not copied.
    void Record(const char* name, std::uint64_t id = 0, std::int64_t a = 0, std::int64_t b = 0, std::int64_t c = 0,
                std::int64_t d = 0, std::int64_t e = 0, std::int64_t f = 0);
    // a, b, c: position in micrometres; d, e, f: orientation x, y, z times
    // 1e6, signed so that w >= 0.
    void RecordPose(const char* name, std::uint64_t id, const XrPosef& pose);

  private:
    struct Event {
        const char* name{nullptr};
        std::int64_t ns{0};
        std::uint32_t thread{0};
        std::uint64_t id{0};
        std::int64_t a{0}, b{0}, c{0}, d{0}, e{0}, f{0};
    };
    void Run();

    static constexpr std::size_t kCapacity = 16384;
    static constexpr std::uint64_t kMaximumEvents = 4000000; // about 400 MB of text

    std::mutex lifecycle_mutex_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::thread worker_;
    std::atomic<bool> enabled_{false};
    bool stopping_{false};
    std::vector<Event> queue_;
    std::uint64_t accepted_{0};
    std::uint64_t dropped_{0};
    std::FILE* file_{nullptr};
    std::chrono::steady_clock::time_point start_{};
};

} // namespace depthxr
