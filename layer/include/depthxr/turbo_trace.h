#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

#include "depthxr/logger.h"

namespace depthxr {

// Debug-only numeric flight recorder. Producers never format, allocate, write
// files, or wait for the consumer. Recent history is continuously persisted to
// two bounded companion logs; sampled detail in the ordinary log is uncapped.
class TurboTimingTrace {
public:
    class Call {
    public:
        ~Call();
        Call(const Call&) = delete;
        Call& operator=(const Call&) = delete;
        Call(Call&&) = delete;
        Call& operator=(Call&&) = delete;
    private:
        friend class TurboTimingTrace;
        Call(TurboTimingTrace& trace, const char* name);
        TurboTimingTrace* trace_{};
        const char* name_{};
        std::size_t slot_{};
        std::uint64_t token_{}, thread_{};
        std::int64_t started_ns_{};
    };

    ~TurboTimingTrace() { Stop(); }
    void Start(Logger& logger);
    // Control-thread calls only. Correlation IDs survive live Debug changes.
    void SetDebugEnabled(Logger& logger, bool enabled);
    void Stop();
    void Burst();
    // Only expensive, optional clock metadata is sampled. Record is continuous.
    bool Capturing();
    bool Enabled() const { return enabled_.load(std::memory_order_relaxed); }
    std::uint64_t NextId() { return Enabled() ? ++next_id_ : 0; }
    [[nodiscard]] Call Watch(const char* name) { return Call(*this, name); }
    void Record(const char* event, std::uint64_t id = 0, std::int64_t a = 0,
                std::int64_t b = 0, std::int64_t c = 0);
    static std::filesystem::path RecorderPath(const std::filesystem::path& log, unsigned part);
    // Frozen pre-incident history: index 0 is the latest snapshot, 1 and 2 the
    // previous ones, so a later benign stall cannot erase an earlier incident.
    static constexpr unsigned kSnapshots = 3;
    static std::filesystem::path SnapshotPath(const std::filesystem::path& log, unsigned index);
    // Frozen Frame Generation history (RequestFrameGenSnapshot): index 0 is
    // the latest snapshot, 1 the previous one. These files are separate from
    // the incident snapshots, so Frame Generation events never replace stall
    // or fault evidence.
    static constexpr unsigned kFrameGenSnapshots = 2;
    static std::filesystem::path FrameGenSnapshotPath(const std::filesystem::path& log, unsigned index);
    // At most one Frame Generation snapshot per interval. Requests in between
    // are recorded and freeze nothing.
    static constexpr std::int64_t kFrameGenSnapshotIntervalNs = 30'000'000'000;
    // Records `event` with a and b, plus c=1 when the event freezes a
    // snapshot or c=0 when the interval has not passed yet. The writer then
    // freezes recent history into the Frame Generation snapshots. The event
    // also goes to the ordinary log. Any thread; never waits. Returns false
    // when nothing was recorded (the recorder is off, or producers held it),
    // so the caller can ask again later.
    bool RequestFrameGenSnapshot(const char* event, std::int64_t a = 0, std::int64_t b = 0);
    // Ordinary-log copies of sampled events per capture session; the recorder
    // files keep recording beyond it.
    static constexpr std::uint64_t kSampledLogLines = 100000;

private:
    friend class TurboTraceTestPeer;
    struct Event {
        const char* name{}; // static literals, never borrowed application memory
        std::int64_t ns{};
        std::uint64_t thread{}, id{}, sequence{};
        std::int64_t a{}, b{}, c{};
        bool sampled{};
        const char* phase{};
    };
    struct PendingCall {
        std::atomic<std::uint64_t> token{0};
        const char* name{};
        std::int64_t started_ns{}, reported_ns{};
        std::uint64_t thread{};
    };
    void Run(Logger& logger);
    void StopLocked();
    void SetDebugEnabledLocked(Logger& logger, bool enabled);
    bool SampledLocked(std::chrono::steady_clock::time_point now);
    void AppendLocked(Event event);
    std::unique_lock<std::mutex> TryProducerLock();
    void BeginCall(Call& call);
    void EndCall(const Call& call);
    std::int64_t NowNs() const;
    static constexpr std::size_t kCapacity = 32768;
    static constexpr std::size_t kPendingCalls = 128;
    static constexpr std::uint64_t kRecorderPartBytes = 4 * 1024 * 1024;
    std::unique_ptr<Event[]> queue_;
    std::array<PendingCall, kPendingCalls> pending_{};
    std::mutex lifecycle_mutex_;
    bool session_active_{false};
    std::mutex mutex_;
    std::condition_variable cv_;
    std::thread worker_;
    std::atomic<bool> enabled_{false};
    std::atomic<std::uint64_t> next_id_{0}, next_call_{0};
    std::atomic<std::uint64_t> dropped_{0}, untracked_calls_{0};
    bool stopping_{false}, writer_failed_{false};
    // A Frame Generation snapshot is due at the writer's next batch, and when
    // the last one was due (both mutex_).
    bool framegen_snapshot_due_{false};
    std::optional<std::int64_t> last_framegen_snapshot_ns_;
    std::int64_t wall_unix_ms_{0}, last_event_ns_{0}, idle_report_ns_{0};
    std::size_t head_{0}, count_{0};
    std::uint64_t sequence_{0};
    std::filesystem::path log_path_;
    std::chrono::steady_clock::time_point start_{}, burst_until_{}, next_burst_{};
};

// Decides which Frame Generation events ask for a snapshot
// (RequestFrameGenSnapshot); the layer polls it from its config watcher.
// Poll takes the session's Dynamic stand-downs plus probes so far and whether
// runtime halving is reported. The first poll after Reset, and a count lower
// than the last one (a new session reset it), set the baseline and ask
// nothing: those events are not this recording's to freeze. After that, a
// rise in the count and halving turning on each call request once. A request
// that returns false (the recorder could not take it) is asked again at the
// next poll; one that returns true counts as made, even when the rate limit
// froze nothing (c=0).
class FrameGenSnapshotTrigger {
public:
    enum class Event { kStandDown, kHalving };
    void Reset() { seen_ = false; }
    // request: bool(Event).
    template <typename Request>
    void Poll(std::uint64_t stand_downs, bool halving, Request&& request) {
        if (!seen_ || stand_downs < stand_downs_) {
            seen_ = true;
            stand_downs_ = stand_downs;
            halving_ = halving;
            return;
        }
        if (stand_downs > stand_downs_) {
            if (!request(Event::kStandDown)) return;
            stand_downs_ = stand_downs;
        }
        if (halving && !halving_) {
            if (!request(Event::kHalving)) return;
        }
        halving_ = halving;
    }

private:
    bool seen_{false};
    std::uint64_t stand_downs_{0};
    bool halving_{false};
};
} // namespace depthxr
