#include "depthxr/turbo_trace.h"

#include <algorithm>
#include <vector>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

using namespace std::chrono_literals;
namespace depthxr {
class TurboTraceTestPeer {
public:
    static std::unique_lock<std::mutex> Lock(TurboTimingTrace& trace) {
        return std::unique_lock(trace.mutex_);
    }
    static std::uint64_t Dropped(const TurboTimingTrace& trace) { return trace.dropped_.load(); }
    // Records waiting for the writer reach half the queue, where it is woken.
    static bool QueueHalfFull(TurboTimingTrace& trace) {
        std::scoped_lock lock(trace.mutex_);
        return trace.count_ >= TurboTimingTrace::kCapacity / 2;
    }
    static std::size_t Active(const TurboTimingTrace& trace) {
        std::size_t active = 0;
        for (const auto& p : trace.pending_) if (p.token.load()) ++active;
        return active;
    }
    // The producers last recorded three seconds ago: the writer sees them idle.
    static void AgeIdle(TurboTimingTrace& trace) {
        std::scoped_lock lock(trace.mutex_);
        trace.last_event_ns_ -= 3'000'000'000;
    }
    // The producers' current stretch of activity began 20 s earlier.
    static void AgeActivity(TurboTimingTrace& trace) {
        std::scoped_lock lock(trace.mutex_);
        trace.active_since_ns_ -= 20'000'000'000;
    }
    static void AgeCalls(TurboTimingTrace& trace) {
        std::scoped_lock lock(trace.mutex_);
        for (auto& p : trace.pending_) if (p.token.load()) p.started_ns -= 3'000'000'000;
    }
    static void EndBurst(TurboTimingTrace& trace) {
        std::scoped_lock lock(trace.mutex_);
        trace.burst_until_ = std::chrono::steady_clock::now() - 1s;
        trace.next_burst_ = std::chrono::steady_clock::now() + 1h;
    }
    // The last Frame Generation snapshot happened one interval earlier.
    static void AgeFrameGenSnapshot(TurboTimingTrace& trace) {
        std::scoped_lock lock(trace.mutex_);
        if (trace.last_framegen_snapshot_ns_)
            *trace.last_framegen_snapshot_ns_ -= TurboTimingTrace::kFrameGenSnapshotIntervalNs;
    }
};
}
namespace {
void Check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
std::string Read(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), {});
}
std::string History(const std::filesystem::path& log) {
    return Read(depthxr::TurboTimingTrace::RecorderPath(log, 0)) +
           Read(depthxr::TurboTimingTrace::RecorderPath(log, 1));
}
template <typename Predicate> bool Wait(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + 4s;
    do { if (predicate()) return true; std::this_thread::sleep_for(20ms); }
    while (std::chrono::steady_clock::now() < deadline);
    return false;
}
std::filesystem::path Snapshot(const std::filesystem::path& log) {
    auto result = log; result.replace_extension(".flight-recorder.stall.log"); return result;
}
double ProcessCpuMs() {
#ifdef _WIN32
    FILETIME created{}, ended{}, kernel{}, user{};
    if (GetProcessTimes(GetCurrentProcess(), &created, &ended, &kernel, &user)) {
        const auto ticks = [](FILETIME t) { return (static_cast<std::uint64_t>(t.dwHighDateTime) << 32) | t.dwLowDateTime; };
        return static_cast<double>(ticks(kernel) + ticks(user)) / 10000.0;
    }
#endif
    return 0;
}
void Benchmark(const std::filesystem::path& root) {
    std::filesystem::create_directories(root);
    depthxr::Logger logger;
    logger.Initialize(root / "benchmark.log");
    depthxr::TurboTimingTrace trace;
    trace.Start(logger);
    for (bool enabled : {false, true}) {
        logger.SetLevel(enabled ? depthxr::LogLevel::Debug : depthxr::LogLevel::Info);
        trace.SetDebugEnabled(logger, enabled);
        const auto run = [&] {
            std::vector<double> samples;
            samples.reserve(360);
            const auto start = std::chrono::steady_clock::now();
            for (int frame = 0; frame < 360; ++frame) {
                const auto before = std::chrono::steady_clock::now();
                for (int call_index = 0; call_index < 6; ++call_index) {
                    const auto call = trace.Watch("benchmark.call");
                    trace.Record("benchmark.event", trace.NextId(), frame, call_index);
                }
                for (int i = 0; i < 14; ++i) trace.Record("benchmark.numeric", trace.NextId(), frame, i);
                samples.push_back(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - before).count());
                std::this_thread::sleep_until(start + std::chrono::nanoseconds((frame + 1) * 1'000'000'000LL / 72));
            }
            return samples;
        };
        const auto cpu = ProcessCpuMs();
        auto second = std::async(std::launch::async, run);
        auto samples = run();
        auto others = second.get();
        samples.insert(samples.end(), others.begin(), others.end());
        std::sort(samples.begin(), samples.end());
        double sum = 0;
        for (const auto sample : samples) sum += sample;
        std::cout << (enabled ? "Debug" : "Info") << ": 2 producers x 72Hz x 32 records/frame; producer us/frame avg="
                  << sum / samples.size() << " p95=" << samples[samples.size() * 95 / 100]
                  << " max=" << samples.back() << "; process CPU ms/5s=" << ProcessCpuMs() - cpu << '\n';
    }
    trace.Stop();
    std::cout << "Benchmark history: " << logger.ActiveLogPath().string() << '\n';
}
void TestContinuousPersistenceAndStall(const std::filesystem::path& root) {
    depthxr::Logger logger;
    logger.Initialize(root / "watchdog.log");
    logger.SetLevel(depthxr::LogLevel::Debug);
    depthxr::TurboTimingTrace trace;
    trace.Start(logger);
    const auto log = logger.ActiveLogPath();
    trace.Record("before.stall", trace.NextId(), 123);
    depthxr::TurboTraceTestPeer::EndBurst(trace);
    Check(!trace.Capturing(), "Expensive metadata was not kept sampled");
    trace.Record("outside.burst", trace.NextId(), 456);
    {
        const auto call = trace.Watch("runtime.wait");
        depthxr::TurboTraceTestPeer::AgeCalls(trace);
        // No new frame records and no Stop: the consumer must diagnose and
        // durably preserve the outstanding call by itself.
        Check(Wait([&] { return Read(Snapshot(log)).find("phase=stall") != std::string::npos; }),
              "Independent watchdog did not preserve a stalled call");
        const auto frozen = Read(Snapshot(log));
        Check(frozen.find("event=before.stall a=123") != std::string::npos &&
              frozen.find("event=outside.burst a=456") != std::string::npos &&
              frozen.find("event=runtime.wait") != std::string::npos &&
              frozen.find("phase=enter") != std::string::npos,
              "Frozen snapshot lacks continuous pre-stall history");
        Check(Read(log).find("event=outside.burst") == std::string::npos,
              "Unsampled history flooded the ordinary log");
        // The recorder holds only records: they leave out the prefix the
        // ordinary log's sampled copies carry (config_tests reads those).
        Check(Read(Snapshot(log)).rfind("ns=", 0) == 0 &&
                  Read(Snapshot(log)).find("Turbo-trace") == std::string::npos,
              "Recorder lines carry the ordinary log's prefix");
    }
    depthxr::TurboTraceTestPeer::EndBurst(trace);
    const auto frozen = Read(Snapshot(log));
    // Exercise both disk rollovers. The producer waits while the queue is
    // half full (the writer is woken then) instead of sleeping: a Windows
    // sleep lasts a 15.6 ms timer tick, about 6 s of this test's 15 s.
    for (int i = 0; i < 100000; ++i) {
        trace.Record("after.stall", trace.NextId(), i);
        if (i % 1024 == 0) {
            while (depthxr::TurboTraceTestPeer::QueueHalfFull(trace)) std::this_thread::yield();
        }
    }
    // A producer drops a record rather than wait while the writer holds the
    // queue (what failed on CI): record the marker until the writer has it.
    Check(Wait([&] {
              trace.Record("latest.marker", trace.NextId());
              return History(log).find("event=latest.marker") != std::string::npos;
          }),
          "Newest history did not drain");
    trace.Stop();
    Check(Read(Snapshot(log)) == frozen, "Later frames overwrote frozen stall evidence");
    for (unsigned part = 0; part < 2; ++part) {
        const auto path = depthxr::TurboTimingTrace::RecorderPath(log, part);
        Check(std::filesystem::exists(path) && std::filesystem::file_size(path) <= 4 * 1024 * 1024,
              "Recorder disk history exceeded its fixed bounds");
    }
}
void TestProducersNeverWait(const std::filesystem::path& root) {
    depthxr::Logger logger;
    logger.Initialize(root / "contention.log");
    logger.SetLevel(depthxr::LogLevel::Debug);
    depthxr::TurboTimingTrace trace;
    trace.Start(logger);
    {
        auto lock = depthxr::TurboTraceTestPeer::Lock(trace);
        auto producer = std::async(std::launch::async, [&] {
            for (int i = 0; i < 100000; ++i) trace.Record("contended", trace.NextId());
            const auto call = trace.Watch("contended.call");
        });
        const bool finished = producer.wait_for(1s) == std::future_status::ready;
        const auto lost = depthxr::TurboTraceTestPeer::Dropped(trace);
        lock.unlock();
        producer.get();
        Check(finished && lost >= 100000, "Producer waited for the consumer instead of reporting dropped records");
    }
    std::unique_lock<std::mutex> held;
    bool tested = false;
    for (int attempt = 0; attempt < 100 && !tested; ++attempt) {
        {
            const auto call = trace.Watch("completed.call");
            if (depthxr::TurboTraceTestPeer::Active(trace) == 1) {
                held = depthxr::TurboTraceTestPeer::Lock(trace);
                tested = true;
                // Destructor runs while this thread holds the consumer lock.
            }
        }
        if (!tested) std::this_thread::sleep_for(1ms);
    }
    Check(tested, "No uncontended call was available for the completion test");
    Check(depthxr::TurboTraceTestPeer::Active(trace) == 0,
          "Dropped completion left a phantom outstanding call");
    held.unlock();
    trace.Stop();
    Check(History(logger.ActiveLogPath()).find("event=records.dropped") != std::string::npos,
          "History loss was not disclosed");
}
void TestDebugControlAndFailure(const std::filesystem::path& root) {
    depthxr::Logger logger;
    logger.Initialize(root / "toggle.log");
    const auto path = logger.ActiveLogPath();
    depthxr::TurboTimingTrace trace;
    trace.Start(logger);
    trace.Record("disabled");
    Check(!trace.Enabled() && !std::filesystem::exists(depthxr::TurboTimingTrace::RecorderPath(path, 0)),
          "Info created a recorder");
    logger.SetLevel(depthxr::LogLevel::Debug);
    trace.SetDebugEnabled(logger, true);
    const auto id = trace.NextId();
    trace.Record("debug.first", id);
    {
        const auto previous = trace.Watch("old.debug.call");
        logger.SetLevel(depthxr::LogLevel::Info);
        trace.SetDebugEnabled(logger, false);
        Check(depthxr::TurboTraceTestPeer::Active(trace) == 0, "Paused Debug retained outstanding calls");
        trace.Record("disabled.after.pause");
        logger.SetLevel(depthxr::LogLevel::Debug);
        trace.SetDebugEnabled(logger, true);
        Check(trace.NextId() > id, "Debug resume reset correlation IDs");
        trace.Record("debug.resumed");
    }
    trace.Stop();
    trace.SetDebugEnabled(logger, true);
    Check(!trace.Enabled(), "Debug update restarted a stopped session");
    const auto history = History(path);
    Check(history.find("event=debug.resumed") != std::string::npos &&
          history.find("event=disabled") == std::string::npos,
          "Debug control leaked Info records or lost the resumed tail");

    depthxr::Logger failure_logger;
    failure_logger.Initialize(root / "failure.log");
    failure_logger.SetLevel(depthxr::LogLevel::Debug);
    const auto blocked = depthxr::TurboTimingTrace::RecorderPath(failure_logger.ActiveLogPath(), 0);
    std::filesystem::create_directory(blocked); // deterministic open failure
    depthxr::TurboTimingTrace failure;
    failure.Start(failure_logger);
    Check(Wait([&] { return !failure.Enabled(); }), "Writer failure did not disable diagnostics");
    failure.Stop(); // cannot throw/terminate the host
}
void TestIncidentSnapshotsRotate(const std::filesystem::path& root) {
    depthxr::Logger logger;
    logger.Initialize(root / "incident.log");
    logger.SetLevel(depthxr::LogLevel::Debug);
    depthxr::TurboTimingTrace trace;
    trace.Start(logger);
    const auto log = logger.ActiveLogPath();
    const auto latest = depthxr::TurboTimingTrace::SnapshotPath(log, 0);
    const auto previous = depthxr::TurboTimingTrace::SnapshotPath(log, 1);
    trace.Record("before.fault", trace.NextId(), 11);
    trace.Record("fault.begin", 0, -37);
    // A fault freezes history at once; the application keeps calling and
    // would otherwise rotate it away.
    Check(Wait([&] { return Read(latest).find("event=fault.begin a=-37") != std::string::npos; }),
          "A fault did not freeze recorder history");
    Check(Read(latest).find("event=before.fault a=11") != std::string::npos,
          "The fault snapshot lacks pre-fault history");
    // A fault repeating every frame must not turn into repeated 8 MiB copies.
    trace.Record("fault.begin", 0, -37);
    std::this_thread::sleep_for(600ms);
    Check(!std::filesystem::exists(previous), "Repeated faults were snapshotted without a rate limit");
    // A later stall keeps the fault snapshot as the previous incident.
    {
        const auto call = trace.Watch("runtime.wait");
        depthxr::TurboTraceTestPeer::AgeCalls(trace);
        Check(Wait([&] { return Read(latest).find("phase=stall") != std::string::npos; }),
              "A stall after a fault was not frozen");
    }
    Check(Read(previous).find("event=fault.begin") != std::string::npos,
          "A later stall overwrote the earlier fault snapshot");
    Check(!std::filesystem::exists(depthxr::TurboTimingTrace::SnapshotPath(log, 2)),
          "Snapshot rotation created a snapshot without an incident");
    trace.Stop();
}
// Games stop calling OpenXR while they load. Only an idle that follows a
// sustained stretch of calls freezes history: not the first frames before a
// load, and not one frame between two loading pauses.
void TestIdleSnapshotsNeedActivity(const std::filesystem::path& root) {
    depthxr::Logger logger;
    logger.Initialize(root / "idle.log");
    logger.SetLevel(depthxr::LogLevel::Debug);
    depthxr::TurboTimingTrace trace;
    trace.Start(logger);
    const auto log = logger.ActiveLogPath();
    const auto latest = depthxr::TurboTimingTrace::SnapshotPath(log, 0);
    const auto previous = depthxr::TurboTimingTrace::SnapshotPath(log, 1);
    trace.Record("first.frames", trace.NextId(), 1);
    depthxr::TurboTraceTestPeer::AgeIdle(trace);
    Check(Wait([&] { return History(log).find("event=producer.idle") != std::string::npos; }),
          "An idle producer was not reported");
    Check(!std::filesystem::exists(latest), "An idle right after the first frames froze a snapshot");

    trace.Record("steady.frames", trace.NextId(), 2);
    Check(Wait([&] { return History(log).find("event=steady.frames") != std::string::npos; }),
          "Resumed frames were not recorded");
    depthxr::TurboTraceTestPeer::AgeActivity(trace);
    depthxr::TurboTraceTestPeer::AgeIdle(trace);
    Check(Wait([&] { return Read(latest).find("event=steady.frames a=2") != std::string::npos; }),
          "An idle after a sustained stretch of calls did not freeze history");

    trace.Record("one.frame", trace.NextId(), 3);
    Check(Wait([&] { return History(log).find("event=one.frame") != std::string::npos; }),
          "The frame between two idles was not recorded");
    depthxr::TurboTraceTestPeer::AgeIdle(trace);
    std::this_thread::sleep_for(600ms);
    Check(!std::filesystem::exists(previous) && Read(latest).find("event=one.frame") == std::string::npos,
          "One frame between two idles froze a second snapshot");
    trace.Stop();
}
void TestFrameGenSnapshotNames() {
    const std::filesystem::path log = std::filesystem::path("logs") / "vectorxr-20261011-162004.log";
    const auto name = [&](unsigned index) {
        return depthxr::TurboTimingTrace::FrameGenSnapshotPath(log, index).filename().string();
    };
    Check(name(0) == "vectorxr-20261011-162004.flight-recorder.framegen.log" &&
          name(1) == "vectorxr-20261011-162004.flight-recorder.framegen-1.log" &&
          depthxr::TurboTimingTrace::FrameGenSnapshotPath(log, 0).parent_path() == log.parent_path(),
          "Frame Generation snapshots are misnamed");
    for (unsigned index = 0; index < depthxr::TurboTimingTrace::kFrameGenSnapshots; ++index) {
        const auto path = depthxr::TurboTimingTrace::FrameGenSnapshotPath(log, index);
        for (unsigned other = 0; other < depthxr::TurboTimingTrace::kSnapshots; ++other)
            Check(path != depthxr::TurboTimingTrace::SnapshotPath(log, other),
                  "A Frame Generation snapshot shares an incident snapshot's name");
        for (unsigned part = 0; part < 2; ++part)
            Check(path != depthxr::TurboTimingTrace::RecorderPath(log, part),
                  "A Frame Generation snapshot shares a recorder part's name");
    }
}
void TestFrameGenSnapshots(const std::filesystem::path& root) {
    depthxr::Logger logger;
    logger.Initialize(root / "framegen.log");
    depthxr::TurboTimingTrace trace;
    trace.Start(logger);
    Check(!trace.RequestFrameGenSnapshot("framegen.standdown", 1),
          "A Frame Generation snapshot was accepted without Debug");
    logger.SetLevel(depthxr::LogLevel::Debug);
    trace.SetDebugEnabled(logger, true);
    const auto log = logger.ActiveLogPath();
    const auto incident = depthxr::TurboTimingTrace::SnapshotPath(log, 0);
    const auto latest = depthxr::TurboTimingTrace::FrameGenSnapshotPath(log, 0);
    const auto previous = depthxr::TurboTimingTrace::FrameGenSnapshotPath(log, 1);
    // Fault evidence first: Frame Generation snapshots must never replace it.
    trace.Record("fault.begin", 0, -37);
    Check(Wait([&] { return Read(incident).find("event=fault.begin a=-37") != std::string::npos; }),
          "A fault did not freeze recorder history");
    const auto fault = Read(incident);
    trace.Record("presenter.wait", trace.NextId(), 21);
    Check(trace.RequestFrameGenSnapshot("framegen.standdown", 1, 0), "A Frame Generation snapshot was refused");
    Check(Wait([&] { return Read(latest).find("event=framegen.standdown a=1 b=0 c=1") != std::string::npos; }),
          "A Frame Generation request did not freeze recorder history");
    Check(Read(latest).find("event=presenter.wait a=21") != std::string::npos,
          "The Frame Generation snapshot lacks the history before the request");
    Check(Read(incident) == fault && !std::filesystem::exists(depthxr::TurboTimingTrace::SnapshotPath(log, 1)),
          "A Frame Generation snapshot rotated the incident snapshots");
    // Within the interval a request is recorded and freezes nothing.
    Check(trace.RequestFrameGenSnapshot("framegen.halving", 45000, 90000),
          "A rate-limited Frame Generation request was not recorded");
    Check(Wait([&] { return History(log).find("event=framegen.halving a=45000 b=90000 c=0") != std::string::npos; }),
          "A rate-limited Frame Generation request did not record c=0");
    std::this_thread::sleep_for(300ms);
    Check(!std::filesystem::exists(previous) && Read(latest).find("event=framegen.halving") == std::string::npos,
          "Frame Generation snapshots were not rate-limited");
    // After the interval the next request freezes again; the previous
    // snapshot moves to index 1 and the incident snapshot stays.
    depthxr::TurboTraceTestPeer::AgeFrameGenSnapshot(trace);
    trace.Record("presenter.wait", trace.NextId(), 31);
    Check(trace.RequestFrameGenSnapshot("framegen.standdown", 2, 1), "A second Frame Generation snapshot was refused");
    Check(Wait([&] { return Read(latest).find("event=framegen.standdown a=2 b=1 c=1") != std::string::npos; }),
          "A Frame Generation request after the interval did not freeze recorder history");
    Check(Read(previous).find("event=framegen.standdown a=1 b=0 c=1") != std::string::npos &&
          Read(previous).find("event=framegen.standdown a=2") == std::string::npos,
          "The earlier Frame Generation snapshot did not move to index 1");
    Check(!std::filesystem::exists(depthxr::TurboTimingTrace::FrameGenSnapshotPath(log, 2)),
          "Frame Generation snapshots kept more than two");
    Check(Read(incident) == fault && !std::filesystem::exists(depthxr::TurboTimingTrace::SnapshotPath(log, 1)),
          "A later Frame Generation snapshot rotated the incident snapshots");
    // The events reach the ordinary log too.
    Check(Wait([&] { return Read(log).find("event=framegen.standdown a=2 b=1 c=1") != std::string::npos; }),
          "A Frame Generation request was not written to the ordinary log");
    logger.SetLevel(depthxr::LogLevel::Info);
    trace.SetDebugEnabled(logger, false);
    Check(!trace.RequestFrameGenSnapshot("framegen.standdown", 3), "A paused recorder accepted a request");
    trace.Stop();
    Check(!trace.RequestFrameGenSnapshot("framegen.standdown", 4), "A stopped recorder accepted a request");
    Check(!std::filesystem::exists(depthxr::TurboTimingTrace::FrameGenSnapshotPath(log, 0).string() + ".tmp"),
          "A Frame Generation snapshot left its temporary file");
}
// PollFrameGenRecorder's decision: which polls ask for a Frame Generation
// snapshot.
void TestFrameGenSnapshotTrigger() {
    using Event = depthxr::FrameGenSnapshotTrigger::Event;
    depthxr::FrameGenSnapshotTrigger trigger;
    std::vector<Event> asked;
    bool accept = true;
    const auto request = [&](Event event) { asked.push_back(event); return accept; };
    // The first poll sets the baseline: the session's earlier stand-downs
    // and a standing halving report are not this recording's.
    trigger.Poll(3, true, request);
    trigger.Poll(3, true, request);
    Check(asked.empty(), "The first poll asked for a snapshot of earlier events");
    trigger.Poll(4, true, request);
    trigger.Poll(4, true, request);
    Check(asked == std::vector<Event>{Event::kStandDown},
          "A stand-down or probe did not ask for exactly one snapshot");
    // A new session resets the counts: a lower count is a new baseline.
    asked.clear();
    trigger.Poll(1, false, request);
    Check(asked.empty(), "A count reset asked for a snapshot");
    trigger.Poll(2, false, request);
    Check(asked == std::vector<Event>{Event::kStandDown},
          "A stand-down after a count reset did not ask for a snapshot");
    // A refused request is asked again at the next poll, until taken.
    asked.clear();
    accept = false;
    trigger.Poll(3, false, request);
    trigger.Poll(3, false, request);
    accept = true;
    trigger.Poll(3, false, request);
    trigger.Poll(3, false, request);
    Check(asked == std::vector<Event>(3, Event::kStandDown),
          "A refused request was not asked again, or a taken one was asked twice");
    // Halving asks when it turns on, not while it stands or when it clears:
    // false -> true -> false -> true asks twice.
    asked.clear();
    trigger.Poll(3, true, request);
    trigger.Poll(3, true, request);
    trigger.Poll(3, false, request);
    trigger.Poll(3, true, request);
    Check(asked == std::vector<Event>(2, Event::kHalving),
          "Halving turning on twice did not ask for exactly two snapshots");
    // Both in one poll: a taken stand-down is not asked again while a refused
    // halving request is.
    trigger.Poll(3, false, request);
    asked.clear();
    trigger.Poll(4, true, [&](Event event) { asked.push_back(event); return event == Event::kStandDown; });
    trigger.Poll(4, true, request);
    trigger.Poll(4, true, request);
    Check(asked == std::vector<Event>{Event::kStandDown, Event::kHalving, Event::kHalving},
          "A refused halving request was not asked again, or its taken stand-down was");
    // Reset (Debug or Frame Generation off) makes the next poll a baseline.
    asked.clear();
    trigger.Reset();
    trigger.Poll(6, false, request);
    trigger.Poll(6, false, request);
    Check(asked.empty(), "The first poll after Reset asked for a snapshot");
}
// With the recorder: a request it records but rate-limits (c=0) counts as
// made and is not asked again.
void TestFrameGenSnapshotTriggerRateLimit(const std::filesystem::path& root) {
    depthxr::Logger logger;
    logger.Initialize(root / "framegen-trigger.log");
    logger.SetLevel(depthxr::LogLevel::Debug);
    depthxr::TurboTimingTrace trace;
    trace.Start(logger);
    const auto log = logger.ActiveLogPath();
    depthxr::FrameGenSnapshotTrigger trigger;
    std::uint64_t asked = 0, taken = 0;
    const auto request = [&](depthxr::FrameGenSnapshotTrigger::Event) {
        ++asked;
        const bool recorded = trace.RequestFrameGenSnapshot("framegen.standdown", 70 + static_cast<std::int64_t>(taken));
        if (recorded) ++taken;
        return recorded;
    };
    trigger.Poll(0, false, request);
    // A producer holding the recorder only moves a request to a later poll.
    for (std::uint64_t stand_downs = 1; stand_downs <= 2; ++stand_downs)
        for (int poll = 0; poll < 10 && taken < stand_downs; ++poll) trigger.Poll(stand_downs, false, request);
    Check(taken == 2, "The recorder did not take both Frame Generation requests");
    const auto handled = asked;
    trigger.Poll(2, false, request);
    trigger.Poll(2, false, request);
    Check(asked == handled, "A rate-limited request the recorder took was asked again");
    trace.Stop();
    const auto history = History(log);
    Check(history.find("event=framegen.standdown a=70 b=0 c=1") != std::string::npos &&
          history.find("event=framegen.standdown a=71 b=0 c=0") != std::string::npos,
          "The second request was not the rate-limited one (c=0)");
}
void TestReopenedPartStaysBounded(const std::filesystem::path& root) {
    depthxr::Logger logger;
    logger.Initialize(root / "reopen.log");
    logger.SetLevel(depthxr::LogLevel::Debug);
    const auto log = logger.ActiveLogPath();
    const auto part0 = depthxr::TurboTimingTrace::RecorderPath(log, 0);
    // An earlier session in the same process left part 0 at its bound.
    {
        std::ofstream prior(part0, std::ios::binary);
        prior << std::string(4 * 1024 * 1024, 'x');
    }
    const auto prior_size = std::filesystem::file_size(part0);
    depthxr::TurboTimingTrace trace;
    trace.Start(logger);
    for (int i = 0; i < 1000; ++i) trace.Record("second.session", trace.NextId(), i);
    trace.Stop();
    Check(std::filesystem::file_size(part0) == prior_size,
          "A reopened recorder part grew past its bound");
    Check(Read(depthxr::TurboTimingTrace::RecorderPath(log, 1)).find("event=second.session") != std::string::npos,
          "The second session's history was not rotated into the other part");
}
// Recorder threads are the OS's thread ids, as the Threads census and the
// Frame Generation trace print them, so the three can be joined.
void TestThreadIdsAreOsThreadIds(const std::filesystem::path& root) {
#ifdef _WIN32
    depthxr::Logger logger;
    logger.Initialize(root / "threads.log");
    logger.SetLevel(depthxr::LogLevel::Debug);
    depthxr::TurboTimingTrace trace;
    trace.Start(logger);
    // Producers drop a record rather than wait for the writer: record a few.
    for (int i = 0; i < 20; ++i) trace.Record("main.thread", trace.NextId());
    DWORD worker_id = 0;
    std::thread([&] {
        worker_id = GetCurrentThreadId();
        for (int i = 0; i < 20; ++i) const auto call = trace.Watch("worker.call");
    }).join();
    trace.Stop();
    const auto history = History(logger.ActiveLogPath());
    const auto has_line = [&](const std::string& thread, const std::string& event) {
        std::istringstream lines(history);
        for (std::string line; std::getline(lines, line);) {
            if (line.find(" thread=" + thread + " ") != std::string::npos && line.find(event) != std::string::npos)
                return true;
        }
        return false;
    };
    Check(has_line(std::to_string(GetCurrentThreadId()), "event=main.thread"),
          "A record's thread is not its OS thread id");
    Check(has_line(std::to_string(worker_id), "event=worker.call"),
          "A watched call's thread is not its OS thread id");
#else
    (void)root;
#endif
}
// A retention above the default keeps that many logs: the session log does
// not prune to the default before the config is read. Settings reloads with
// the same retention do not rescan the folder; a later session prunes to the
// configured value.
void TestRetentionFollowsTheConfig(const std::filesystem::path& root) {
    const auto directory = root / "retention-config";
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
    std::filesystem::create_directories(directory);
    const auto old_log = [&](int index) {
        const auto path = directory / ("vectorxr-20000101-0000" + std::string(index < 10 ? "0" : "") +
                                       std::to_string(index) + ".log");
        std::ofstream(path) << "old";
        std::filesystem::last_write_time(path, std::filesystem::file_time_type::clock::now() -
                                                   std::chrono::hours(100 - index));
        return path;
    };
    const auto count_logs = [&] {
        int count = 0;
        for (const auto& entry : std::filesystem::directory_iterator(directory))
            if (entry.path().extension() == ".log") ++count;
        return count;
    };
    for (int index = 0; index < 10; ++index) old_log(index);
    depthxr::Logger logger;
    logger.Initialize(directory / "vectorxr.log");
    Check(count_logs() == 10, "A session log pruned to the default before the config set the retention");
    logger.SetRetentionFiles(30);
    logger.Info("new session");
    Check(count_logs() == 11, "A retention of 30 did not keep ten older logs");

    logger.SetRetentionFiles(3);
    Check(count_logs() == 3, "Lowering the retention did not prune to it");
    for (int index = 10; index < 14; ++index) old_log(index);
    logger.SetRetentionFiles(3);
    Check(count_logs() == 7, "A settings reload with the same retention rescanned the log folder");
    logger.Initialize(directory / "vectorxr.log");
    Check(count_logs() == 3, "A later session did not prune to the configured retention");
}
// An instance that never asks for a session discards its log, unless an
// error was written to it.
void TestDiscardedSessionLog(const std::filesystem::path& root) {
    const auto directory = root / "discarded";
    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
    depthxr::Logger logger;
    logger.Initialize(directory / "vectorxr.log");
    logger.Info("probe instance");
    const auto probe = logger.ActiveLogPath();
    Check(std::filesystem::exists(probe), "The probe's log was not written");
    Check(logger.DiscardSessionLog() && !std::filesystem::exists(probe), "A probe instance's log was kept");
    logger.Info("after the probe");
    Check(!std::filesystem::exists(probe) && logger.ActiveLogPath().empty(),
          "A discarded log was written to again");
    std::this_thread::sleep_for(1100ms); // a session log name per second
    logger.Initialize(directory / "vectorxr.log");
    logger.Error("instance creation failed");
    const auto failed = logger.ActiveLogPath();
    Check(!logger.DiscardSessionLog() && std::filesystem::exists(failed), "A log holding an error was discarded");
}
void TestRetention(const std::filesystem::path& root) {
    const auto directory = root / "retention";
    std::filesystem::create_directory(directory);
    const auto old = directory / "vectorxr-20000101-000000.log";
    std::ofstream(old) << "old";
    for (unsigned part = 0; part < 2; ++part)
        std::ofstream(depthxr::TurboTimingTrace::RecorderPath(old, part)) << "old recorder";
    for (unsigned index = 0; index < depthxr::TurboTimingTrace::kSnapshots; ++index)
        std::ofstream(depthxr::TurboTimingTrace::SnapshotPath(old, index)) << "old frozen history";
    for (unsigned index = 0; index < depthxr::TurboTimingTrace::kFrameGenSnapshots; ++index)
        std::ofstream(depthxr::TurboTimingTrace::FrameGenSnapshotPath(old, index)) << "old Frame Generation history";
    auto framegen_trace = old;
    framegen_trace.replace_extension(".framegen-trace.csv");
    std::ofstream(framegen_trace) << "old Frame Generation trace";
    std::filesystem::last_write_time(old, std::filesystem::file_time_type::clock::now() - 1h);
    depthxr::Logger logger;
    logger.Initialize(directory / "vectorxr.log");
    logger.Info("new session");
    logger.SetRetentionFiles(1);
    Check(!std::filesystem::exists(old) &&
          !std::filesystem::exists(depthxr::TurboTimingTrace::RecorderPath(old, 0)) &&
          !std::filesystem::exists(depthxr::TurboTimingTrace::RecorderPath(old, 1)) &&
          !std::filesystem::exists(Snapshot(old)) &&
          !std::filesystem::exists(depthxr::TurboTimingTrace::SnapshotPath(old, 1)) &&
          !std::filesystem::exists(depthxr::TurboTimingTrace::SnapshotPath(old, 2)) &&
          !std::filesystem::exists(depthxr::TurboTimingTrace::FrameGenSnapshotPath(old, 0)) &&
          !std::filesystem::exists(depthxr::TurboTimingTrace::FrameGenSnapshotPath(old, 1)) &&
          !std::filesystem::exists(framegen_trace),
          "Log retention orphaned recorder files");
}
}
int main(int argc, char** argv) {
    try {
        if (argc == 3 && std::string(argv[1]) == "--crash-child") {
            const std::filesystem::path root = argv[2];
            std::filesystem::create_directories(root);
            depthxr::Logger logger;
            logger.Initialize(root / "crash.log");
            logger.SetLevel(depthxr::LogLevel::Debug);
            depthxr::TurboTimingTrace trace;
            trace.Start(logger);
            trace.Record("before.hard.crash", trace.NextId(), 789);
            const auto call = trace.Watch("runtime.never.returns");
            std::this_thread::sleep_for(60s);
            return 0;
        }
        if (argc == 3 && std::string(argv[1]) == "--benchmark") {
            Benchmark(argv[2]);
            return 0;
        }
        // In the temporary folder, not the working directory (a run from the
        // repository root left turbo-trace-tests/ in the checkout); removed
        // after a passing run.
        const auto root = std::filesystem::temp_directory_path() /
                          ("vectorxr-turbo-trace-tests-" + std::to_string(GetCurrentProcessId()));
        std::error_code ignored;
        std::filesystem::remove_all(root, ignored);
        std::filesystem::create_directories(root);
        TestContinuousPersistenceAndStall(root);
        TestProducersNeverWait(root);
        TestDebugControlAndFailure(root);
        TestIncidentSnapshotsRotate(root);
        TestIdleSnapshotsNeedActivity(root);
        TestFrameGenSnapshotNames();
        TestFrameGenSnapshots(root);
        TestFrameGenSnapshotTrigger();
        TestFrameGenSnapshotTriggerRateLimit(root);
        TestReopenedPartStaysBounded(root);
        TestThreadIdsAreOsThreadIds(root);
        TestRetentionFollowsTheConfig(root);
        TestDiscardedSessionLog(root);
        TestRetention(root);
        std::cout << "Flight recorder: continuous persistence, rollover, frozen stalls, incident snapshots and "
                     "rotation, Frame Generation snapshots (names, rate limit, rotation apart from incidents, "
                     "trigger baseline, retry and edges), reopened part bounds, nonblocking producers, Debug "
                     "lifecycle, writer failure, and retention passed.\n";
        std::filesystem::remove_all(root, ignored);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
