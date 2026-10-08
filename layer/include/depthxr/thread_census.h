#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace depthxr {

// Which threads call a session's OpenXR entry points. A game's threading decides where VectorXR may use its
// D3D11 immediate context: which thread releases swapchain images, and whether xrWaitFrame runs apart from
// xrEndFrame. Threads are numbered in order of first appearance in the session (T1 is the first one seen).
// Thread-safe; the lock is held only inside these calls.
class ThreadCensus {
  public:
    enum class Call : std::uint8_t {
        CreateSession,
        CreateSwapchain,
        WaitFrame,
        BeginFrame,
        EndFrame,
        AcquireSwapchainImage,
        WaitSwapchainImage,
        ReleaseSwapchainImage,
        Count,
    };

    // A call's threads are reported one by one up to this many; one more line then says there are more.
    static constexpr std::size_t kReportedThreadsPerCall = 4;
    // Threads beyond this many in a session are not tracked; the summary counts their calls separately.
    static constexpr std::size_t kMaxThreads = 64;

    // Counts `call` on `thread_id`. Returns a line to log when the call arrives on a thread it has not been seen
    // on before (within the reporting limit), and an empty string otherwise. `describe` names a thread new to
    // the census (an empty name is fine). `call_count`, if given, receives the call's count so far.
    std::string Note(Call call,
                     std::uint32_t thread_id,
                     const std::function<std::string()>& describe = {},
                     std::uint64_t* call_count = nullptr);

    // The session's threads and, per call seen, its threads with their counts.
    [[nodiscard]] std::string Summary() const;
    [[nodiscard]] std::uint64_t Count(Call call) const;
    void Reset();

  private:
    struct Thread {
        std::uint32_t id{0};
        std::string name;
    };
    struct CallThread {
        std::size_t thread{0}; // index into threads_
        std::uint64_t count{0};
    };
    static constexpr std::size_t kCalls = static_cast<std::size_t>(Call::Count);

    [[nodiscard]] std::string SummaryLocked() const;

    mutable std::mutex mutex_;
    std::vector<Thread> threads_;
    std::array<std::vector<CallThread>, kCalls> calls_{};
    std::array<std::uint64_t, kCalls> totals_{};
    std::array<bool, kCalls> overflow_reported_{};
};

const char* ThreadCensusCallName(ThreadCensus::Call call);

} // namespace depthxr
