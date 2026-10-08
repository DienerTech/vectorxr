#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
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
    std::int64_t wall_unix_ms_{0}, last_event_ns_{0}, idle_report_ns_{0};
    std::size_t head_{0}, count_{0};
    std::uint64_t sequence_{0};
    std::filesystem::path log_path_;
    std::chrono::steady_clock::time_point start_{}, burst_until_{}, next_burst_{};
};
} // namespace depthxr
