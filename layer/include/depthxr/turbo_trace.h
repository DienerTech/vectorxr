#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

#include "depthxr/logger.h"

namespace depthxr {

// Debug-level forensic bursts. Producers only copy numeric records into a bounded
// queue; formatting and file writes belong to the dedicated consumer.
class TurboTimingTrace {
public:
    ~TurboTimingTrace() { Stop(); }
    void Start(Logger& logger);
    // Control-thread calls only; preserves the session clock and event budget.
    void SetDebugEnabled(Logger& logger, bool enabled);
    void Stop();
    void Burst();
    bool Enabled() const { return enabled_.load(std::memory_order_relaxed); }
    bool Capturing();
    std::uint64_t NextId() { return Enabled() ? ++next_id_ : 0; }
    void Record(const char* event, std::uint64_t id = 0, std::int64_t a = 0,
                std::int64_t b = 0, std::int64_t c = 0);
private:
    struct Event {
        const char* name{}; // static string literal, never borrowed dynamic text
        std::int64_t ns{};
        std::uint64_t thread{}, id{};
        std::int64_t a{}, b{}, c{};
    };
    void Run(Logger& logger);
    void StopLocked();
    void SetDebugEnabledLocked(Logger& logger, bool enabled);
    static constexpr std::size_t kCapacity = 2048;
    static constexpr std::uint64_t kBudget = 30000;
    std::array<Event, kCapacity> queue_{};
    std::mutex lifecycle_mutex_;
    bool session_active_{false};
    std::mutex mutex_;
    std::condition_variable cv_;
    std::thread worker_;
    std::atomic<bool> enabled_{false};
    std::atomic<std::uint64_t> next_id_{0};
    bool stopping_{false};
    bool writer_failed_{false};
    std::int64_t wall_unix_ms_{0};
    std::size_t count_{0};
    std::uint64_t accepted_{0}, dropped_{0};
    std::chrono::steady_clock::time_point start_{}, burst_until_{}, next_burst_{};
};
} // namespace depthxr
