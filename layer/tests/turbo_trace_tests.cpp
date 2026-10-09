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
    static std::size_t Active(const TurboTimingTrace& trace) {
        std::size_t active = 0;
        for (const auto& p : trace.pending_) if (p.token.load()) ++active;
        return active;
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
    }
    depthxr::TurboTraceTestPeer::EndBurst(trace);
    const auto frozen = Read(Snapshot(log));
    // Exercise both disk rollovers. Steady producers keep the consumer ahead.
    for (int i = 0; i < 100000; ++i) {
        trace.Record("after.stall", trace.NextId(), i);
        if (i % 256 == 0) std::this_thread::sleep_for(2ms);
    }
    trace.Record("latest.marker", trace.NextId());
    trace.Stop();
    Check(History(log).find("event=latest.marker") != std::string::npos,
          "Newest history did not drain");
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
void TestRetention(const std::filesystem::path& root) {
    const auto directory = root / "retention";
    std::filesystem::create_directory(directory);
    const auto old = directory / "vectorxr-20000101-000000.log";
    std::ofstream(old) << "old";
    for (unsigned part = 0; part < 2; ++part)
        std::ofstream(depthxr::TurboTimingTrace::RecorderPath(old, part)) << "old recorder";
    for (unsigned index = 0; index < depthxr::TurboTimingTrace::kSnapshots; ++index)
        std::ofstream(depthxr::TurboTimingTrace::SnapshotPath(old, index)) << "old frozen history";
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
          !std::filesystem::exists(depthxr::TurboTimingTrace::SnapshotPath(old, 2)),
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
        const auto root = std::filesystem::current_path() / "turbo-trace-tests";
        std::filesystem::create_directories(root);
        TestContinuousPersistenceAndStall(root);
        TestProducersNeverWait(root);
        TestDebugControlAndFailure(root);
        TestIncidentSnapshotsRotate(root);
        TestReopenedPartStaysBounded(root);
        TestRetention(root);
        std::cout << "Flight recorder: continuous persistence, rollover, frozen stalls, incident snapshots and "
                     "rotation, reopened part bounds, nonblocking producers, Debug lifecycle, writer failure, "
                     "and retention passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
