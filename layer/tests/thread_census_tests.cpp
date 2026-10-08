#include "depthxr/thread_census.h"

#include <cstdlib>
#include <iostream>
#include <string>

using depthxr::ThreadCensus;

namespace {

void Expect(bool value, const std::string& message) {
    if (!value) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

bool Contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

} // namespace

int main() {
    ThreadCensus census;
    int described = 0;
    const auto describe = [&described](const std::string& name) {
        return [&described, name] {
            ++described;
            return name;
        };
    };

    // The first call on the first thread: numbered T1, named once.
    std::string line = census.Note(ThreadCensus::Call::CreateSession, 100, describe("main"));
    Expect(Contains(line, "xrCreateSession on T1 (thread 100 \"main\")"), "first thread line: " + line);
    Expect(described == 1, "a new thread is described");

    // A second thread for a second call.
    line = census.Note(ThreadCensus::Call::WaitFrame, 200, describe(""));
    Expect(Contains(line, "xrWaitFrame on T2 (thread 200)"), "unnamed thread line: " + line);

    // Repeats are counted, not reported.
    std::uint64_t count = 0;
    line = census.Note(ThreadCensus::Call::WaitFrame, 200, describe("unused"), &count);
    Expect(line.empty() && count == 2, "a repeat is silent and counted");

    // A known thread for a new call: reported without describing the thread again.
    const int described_before = described;
    line = census.Note(ThreadCensus::Call::EndFrame, 100, describe("unused"));
    Expect(Contains(line, "xrEndFrame on T1;"), "known thread, new call: " + line);
    Expect(described == described_before, "a known thread is not described again");

    // Further threads for one call: "also on", up to the limit, then one line saying there are more.
    Expect(Contains(census.Note(ThreadCensus::Call::WaitFrame, 300, describe("")), "xrWaitFrame also on T3"),
           "second thread of a call");
    Expect(Contains(census.Note(ThreadCensus::Call::WaitFrame, 400, describe("")), "also on T4"), "third thread");
    Expect(Contains(census.Note(ThreadCensus::Call::WaitFrame, 500, describe("")), "also on T5"), "fourth thread");
    line = census.Note(ThreadCensus::Call::WaitFrame, 600, describe(""));
    Expect(Contains(line, "xrWaitFrame is on more than 4 threads"), "overflow line: " + line);
    Expect(census.Note(ThreadCensus::Call::WaitFrame, 700, describe("")).empty(), "after the overflow line: silence");
    Expect(census.Count(ThreadCensus::Call::WaitFrame) == 7, "every call is counted");

    // The summary maps threads to ids and lists each call's threads with counts.
    const std::string summary = census.Summary();
    Expect(Contains(summary, "T1=100 \"main\", T2=200"), "thread map: " + summary);
    Expect(Contains(summary, "xrCreateSession T1x1"), "session call: " + summary);
    Expect(Contains(summary, "xrWaitFrame T2x2 T3x1"), "wait calls: " + summary);
    Expect(Contains(summary, "xrEndFrame T1x1"), "end calls: " + summary);
    Expect(!Contains(summary, "xrBeginFrame"), "calls never made are left out: " + summary);

    // Beyond kMaxThreads, threads are not tracked, but the summary still accounts for their calls.
    ThreadCensus crowded;
    for (std::uint32_t thread = 1; thread <= ThreadCensus::kMaxThreads + 3; ++thread) {
        crowded.Note(ThreadCensus::Call::AcquireSwapchainImage, thread);
    }
    const std::string crowded_summary = crowded.Summary();
    Expect(crowded.Count(ThreadCensus::Call::AcquireSwapchainImage) == ThreadCensus::kMaxThreads + 3,
           "untracked calls are counted");
    Expect(Contains(crowded_summary, " +3 on untracked threads"), "untracked calls summarised: " + crowded_summary);

    // Reset starts a new session's census: numbering and reporting begin again.
    census.Reset();
    Expect(census.Count(ThreadCensus::Call::WaitFrame) == 0, "reset clears counts");
    line = census.Note(ThreadCensus::Call::ReleaseSwapchainImage, 900, describe("render"));
    Expect(Contains(line, "xrReleaseSwapchainImage on T1 (thread 900 \"render\")"), "after reset: " + line);

    std::cout << "thread census tests passed\n";
    return 0;
}
