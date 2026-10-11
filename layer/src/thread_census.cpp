#include "depthxr/thread_census.h"

#include <algorithm>

namespace depthxr {

const char* ThreadCensusCallName(ThreadCensus::Call call) {
    switch (call) {
    case ThreadCensus::Call::CreateSession:
        return "xrCreateSession";
    case ThreadCensus::Call::CreateSwapchain:
        return "xrCreateSwapchain";
    case ThreadCensus::Call::WaitFrame:
        return "xrWaitFrame";
    case ThreadCensus::Call::BeginFrame:
        return "xrBeginFrame";
    case ThreadCensus::Call::EndFrame:
        return "xrEndFrame";
    case ThreadCensus::Call::AcquireSwapchainImage:
        return "xrAcquireSwapchainImage";
    case ThreadCensus::Call::WaitSwapchainImage:
        return "xrWaitSwapchainImage";
    case ThreadCensus::Call::ReleaseSwapchainImage:
        return "xrReleaseSwapchainImage";
    case ThreadCensus::Call::Count:
        break;
    }
    return "unknown";
}

std::string ThreadCensus::Note(Call call,
                               std::uint32_t thread_id,
                               const std::function<std::string()>& describe,
                               std::uint64_t* call_count) {
    const auto call_index = static_cast<std::size_t>(call);
    if (call_index >= kCalls) {
        return {};
    }
    std::scoped_lock lock(mutex_);
    const std::uint64_t count = ++totals_[call_index];
    if (call_count) {
        *call_count = count;
    }

    auto thread = std::find_if(threads_.begin(), threads_.end(), [&](const Thread& t) { return t.id == thread_id; });
    bool new_thread = false;
    if (thread == threads_.end()) {
        if (threads_.size() >= kMaxThreads) {
            return {};
        }
        threads_.push_back({thread_id, describe ? describe() : std::string()});
        thread = threads_.end() - 1;
        new_thread = true;
    }
    const std::size_t thread_index = static_cast<std::size_t>(thread - threads_.begin());

    auto& seen = calls_[call_index];
    const auto entry =
        std::find_if(seen.begin(), seen.end(), [&](const CallThread& t) { return t.thread == thread_index; });
    if (entry != seen.end()) {
        ++entry->count;
        return {};
    }
    seen.push_back({thread_index, 1});

    const std::string label = "T" + std::to_string(thread_index + 1);
    std::string line = "Threads: ";
    if (seen.size() > kReportedThreadsPerCall) {
        if (overflow_reported_[call_index]) {
            return {};
        }
        overflow_reported_[call_index] = true;
        line += std::string(ThreadCensusCallName(call)) + " is on more than " +
                std::to_string(kReportedThreadsPerCall) + " threads; not reporting further ones";
    } else {
        line += std::string(ThreadCensusCallName(call)) + (seen.size() == 1 ? " on " : " also on ") + label;
        if (new_thread) {
            line += " (thread " + std::to_string(thread_id);
            if (!thread->name.empty()) {
                line += " \"" + thread->name + "\"";
            }
            line += ")";
        }
    }
    return line + "; so far: " + SummaryLocked();
}

std::string ThreadCensus::Summary() const {
    std::scoped_lock lock(mutex_);
    return SummaryLocked();
}

std::string ThreadCensus::SummaryLocked() const {
    std::string summary;
    for (std::size_t i = 0; i < threads_.size(); ++i) {
        summary += (i == 0 ? "T" : ", T") + std::to_string(i + 1) + "=" + std::to_string(threads_[i].id);
        if (!threads_[i].name.empty()) {
            summary += " \"" + threads_[i].name + "\"";
        }
    }
    for (std::size_t c = 0; c < kCalls; ++c) {
        if (totals_[c] == 0) {
            continue;
        }
        summary += std::string(summary.empty() ? "" : "; ") + ThreadCensusCallName(static_cast<Call>(c));
        std::uint64_t tracked = 0;
        for (const CallThread& seen : calls_[c]) {
            summary += " T" + std::to_string(seen.thread + 1) + "x" + std::to_string(seen.count);
            tracked += seen.count;
        }
        if (totals_[c] > tracked) {
            summary += " +" + std::to_string(totals_[c] - tracked) + " on untracked threads";
        }
    }
    return summary;
}

std::uint64_t ThreadCensus::Count(Call call) const {
    const auto call_index = static_cast<std::size_t>(call);
    if (call_index >= kCalls) {
        return 0;
    }
    std::scoped_lock lock(mutex_);
    return totals_[call_index];
}

void ThreadCensus::Reset() {
    std::scoped_lock lock(mutex_);
    threads_.clear();
    for (auto& seen : calls_) {
        seen.clear();
    }
    totals_.fill(0);
    overflow_reported_.fill(false);
}

} // namespace depthxr
