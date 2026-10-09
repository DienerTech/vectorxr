#include "depthxr/turbo_trace.h"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <stdexcept>
#include <vector>

namespace depthxr {
using namespace std::chrono_literals;
namespace {
constexpr std::int64_t kStallNs = 2'000'000'000;
constexpr std::int64_t kReportNs = 5'000'000'000;
std::uint64_t ThreadId() {
    return static_cast<std::uint64_t>(std::hash<std::thread::id>{}(std::this_thread::get_id()));
}
template <typename T> void Number(std::string& line, T value) {
    char buffer[32];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
    line.append(buffer, result.ptr);
}
// Fault, dropped-frame and recovery events freeze history like a stall does:
// the application keeps calling afterwards and would rotate it away.
bool IsIncident(const char* name) {
    const std::string_view event(name);
    return event.starts_with("fault.") || event.starts_with("recover.") || event == "frame.dropped";
}
}

std::filesystem::path TurboTimingTrace::RecorderPath(const std::filesystem::path& log, unsigned part) {
    auto path = log;
    path.replace_extension(".flight-recorder." + std::to_string(part) + ".log");
    return path;
}

std::filesystem::path TurboTimingTrace::SnapshotPath(const std::filesystem::path& log, unsigned index) {
    auto path = log;
    path.replace_extension(index == 0 ? std::string(".flight-recorder.stall.log")
                                      : ".flight-recorder.stall-" + std::to_string(index) + ".log");
    return path;
}

std::int64_t TurboTimingTrace::NowNs() const {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - start_).count();
}

void TurboTimingTrace::Start(Logger& logger) {
    std::scoped_lock lifecycle_lock(lifecycle_mutex_);
    StopLocked();
    {
        std::scoped_lock lock(mutex_);
        // Allocate on the control path only, and only when Debug is enabled.
        head_ = count_ = 0;
        sequence_ = 0;
        dropped_ = untracked_calls_ = 0;
        next_id_ = 0;
        stopping_ = writer_failed_ = false;
        start_ = std::chrono::steady_clock::now();
        last_event_ns_ = idle_report_ns_ = 0;
        wall_unix_ms_ = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        burst_until_ = start_ + 1s;
        next_burst_ = start_ + 30s;
        for (auto& p : pending_) p.token.store(0, std::memory_order_release);
    }
    log_path_ = logger.ActiveLogPath();
    session_active_ = true;
    SetDebugEnabledLocked(logger, logger.IsDebugEnabled());
}

void TurboTimingTrace::SetDebugEnabled(Logger& logger, bool enabled) {
    std::scoped_lock lifecycle_lock(lifecycle_mutex_);
    SetDebugEnabledLocked(logger, enabled);
}

void TurboTimingTrace::SetDebugEnabledLocked(Logger& logger, bool enabled) {
    if (!session_active_ || enabled == Enabled()) return;
    {
        std::scoped_lock lock(mutex_);
        if (writer_failed_) return;
        enabled_.store(false, std::memory_order_release);
        for (auto& p : pending_) p.token.store(0, std::memory_order_release);
    }
    if (!enabled) {
        cv_.notify_all();
        return;
    }
    if (!worker_.joinable()) {
        try {
            if (!queue_) queue_ = std::make_unique<Event[]>(kCapacity);
            worker_ = std::thread([this, &logger] { Run(logger); });
        } catch (...) {
            std::scoped_lock lock(mutex_);
            writer_failed_ = true;
            logger.Error("Turbo trace: writer could not start; diagnostics disabled.");
            return;
        }
        logger.Debug("Turbo trace v3: continuous recent history in " + RecorderPath(log_path_, 0).string() +
            " and part 1 (4 MiB each); stalls, faults and recoveries freeze the latest three snapshots. "
            "The ordinary log keeps up to 100000 sampled lines per session. "
            "Queue=32768; background flush=250ms; stall threshold=2s. "
            "ns is monotonic since capture start; unixMs anchors exported tails; sequence detects lost history. "
            "phase=enter/exit/stall tracks calls by id; a=call age ns on exit/stall. "
            "Clock metadata remains sampled (1s every 30s and on transitions/errors). "
            "Timing diagnostics do not measure GPU execution or ASW activity.");
    }
    {
        std::scoped_lock lock(mutex_);
        if (writer_failed_) return;
        last_event_ns_ = NowNs();
        idle_report_ns_ = 0;
        burst_until_ = std::chrono::steady_clock::now() + 1s;
        enabled_.store(true, std::memory_order_release);
    }
    cv_.notify_all();
    Record("capture.resume");
}

void TurboTimingTrace::Stop() {
    std::scoped_lock lifecycle_lock(lifecycle_mutex_);
    StopLocked();
}
void TurboTimingTrace::StopLocked() {
    session_active_ = false;
    enabled_.store(false, std::memory_order_release);
    {
        std::scoped_lock lock(mutex_);
        stopping_ = true;
        for (auto& p : pending_) p.token.store(0, std::memory_order_release);
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void TurboTimingTrace::Burst() {
    if (!Enabled()) return;
    std::unique_lock lock(mutex_, std::try_to_lock);
    if (lock) burst_until_ = std::chrono::steady_clock::now() + 1s;
}
bool TurboTimingTrace::Capturing() {
    if (!Enabled()) return false;
    std::unique_lock lock(mutex_, std::try_to_lock);
    const auto now = std::chrono::steady_clock::now();
    return lock && !stopping_ && (now < burst_until_ || now >= next_burst_);
}
bool TurboTimingTrace::SampledLocked(std::chrono::steady_clock::time_point now) {
    if (now >= next_burst_) {
        burst_until_ = now + 1s;
        next_burst_ = now + 30s;
    }
    return now < burst_until_;
}
void TurboTimingTrace::AppendLocked(Event event) {
    event.sequence = ++sequence_;
    if (count_ == kCapacity) {
        head_ = (head_ + 1) % kCapacity; // Keep recent history if the disk falls behind.
        --count_;
        ++dropped_;
    }
    queue_[(head_ + count_) % kCapacity] = event;
    ++count_;
    last_event_ns_ = event.ns;
    if (count_ == kCapacity / 2) cv_.notify_one();
}
std::unique_lock<std::mutex> TurboTimingTrace::TryProducerLock() {
    std::unique_lock lock(mutex_, std::defer_lock);
    // A few immediate attempts handle overlapping producers without sleeping,
    // yielding, or waiting for the disk/consumer. Long contention still drops.
    for (unsigned attempt = 0; attempt < 16; ++attempt) {
        if (lock.try_lock()) break;
    }
    return lock;
}
void TurboTimingTrace::Record(const char* event, std::uint64_t id, std::int64_t a,
                              std::int64_t b, std::int64_t c) {
    if (!Enabled()) return;
    auto lock = TryProducerLock();
    if (!lock) { ++dropped_; return; }
    if (!Enabled() || stopping_) return;
    const auto now = std::chrono::steady_clock::now();
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now - start_).count();
    AppendLocked({event, ns, ThreadId(), id, 0, a, b, c, SampledLocked(now), nullptr});
}

TurboTimingTrace::Call::Call(TurboTimingTrace& trace, const char* name) : name_(name) {
    trace.BeginCall(*this);
}
TurboTimingTrace::Call::~Call() {
    if (trace_) trace_->EndCall(*this);
}
void TurboTimingTrace::BeginCall(Call& call) {
    if (!Enabled()) return;
    auto lock = TryProducerLock();
    if (!lock) { ++untracked_calls_; return; }
    if (!Enabled() || stopping_) return;
    for (std::size_t i = 0; i < pending_.size(); ++i) {
        auto& p = pending_[i];
        if (p.token.load(std::memory_order_acquire)) continue;
        call.trace_ = this;
        call.slot_ = i;
        call.token_ = ++next_call_; // Never reuse a token across pause/session boundaries.
        call.thread_ = ThreadId();
        call.started_ns_ = NowNs();
        p.name = call.name_;
        p.started_ns = call.started_ns_;
        p.reported_ns = 0;
        p.thread = call.thread_;
        p.token.store(call.token_, std::memory_order_release);
        AppendLocked({call.name_, call.started_ns_, call.thread_, call.token_, 0, 0, 0, 0,
                      false, "enter"});
        return;
    }
    ++untracked_calls_;
}
void TurboTimingTrace::EndCall(const Call& call) {
    // Completion never waits for the consumer lock. A dropped exit record must
    // not leave a completed call looking stalled to the watchdog.
    auto expected = call.token_;
    if (!pending_[call.slot_].token.compare_exchange_strong(expected, 0, std::memory_order_acq_rel)) return;
    if (!Enabled()) return;
    auto lock = TryProducerLock();
    if (!lock) { ++dropped_; return; }
    if (!Enabled() || stopping_) return;
    const auto ns = NowNs();
    AppendLocked({call.name_, ns, call.thread_, call.token_, 0, ns - call.started_ns_, 0, 0,
                  false, "exit"});
}

void TurboTimingTrace::Run(Logger& logger) {
    try {
        std::vector<Event> batch;
        batch.reserve(kCapacity + kPendingCalls + 3);
        std::string line;
        line.reserve(256);
        std::string output;
        output.reserve(64 * 1024);
        unsigned part = 0;
        std::uint64_t bytes = 0;
        std::ofstream file;
        const auto open = [&](bool truncate) {
            const auto path = RecorderPath(log_path_, part);
            file.open(path, std::ios::binary | (truncate ? std::ios::trunc : std::ios::app));
            if (!file) throw std::runtime_error("Unable to open flight recorder");
            // An append stream reports position 0 before its first write; use
            // the real size so a part reopened by a later session stays bounded.
            std::error_code size_error;
            const auto size = truncate ? 0 : std::filesystem::file_size(path, size_error);
            bytes = size_error ? 0 : static_cast<std::uint64_t>(size);
        };
        open(false);
        const auto persist = [&](const std::string& text) {
            if (bytes + text.size() > kRecorderPartBytes) {
                file.flush();
                file.close();
                part ^= 1;
                open(true);
            }
            file.write(text.data(), static_cast<std::streamsize>(text.size()));
            bytes += text.size();
            if (!file) throw std::runtime_error("Unable to write flight recorder");
        };
        const auto freeze = [&]() -> bool {
            // Freeze pre-incident history before later healthy frames can rotate
            // it away. The ordinary rolling files continue independently, and a
            // snapshot failure never disables them.
            std::error_code error;
            auto temporary = SnapshotPath(log_path_, 0);
            temporary += ".tmp";
            {
                std::ofstream frozen(temporary, std::ios::binary | std::ios::trunc);
                if (!frozen) return false;
                std::vector<char> copy_buffer(64 * 1024);
                for (unsigned source_part : {part ^ 1, part}) {
                    const auto source = RecorderPath(log_path_, source_part);
                    if (!std::filesystem::exists(source, error)) {
                        error.clear();
                        continue;
                    }
                    std::ifstream history(source, std::ios::binary);
                    if (!history) return false;
                    while (history) {
                        history.read(copy_buffer.data(), static_cast<std::streamsize>(copy_buffer.size()));
                        frozen.write(copy_buffer.data(), history.gcount());
                    }
                    if (!history.eof()) return false;
                }
                frozen.flush();
                if (!frozen) return false;
            }
            for (unsigned index = kSnapshots - 1; index > 0; --index) {
                std::filesystem::rename(SnapshotPath(log_path_, index - 1), SnapshotPath(log_path_, index), error);
                error.clear();
            }
            std::filesystem::rename(temporary, SnapshotPath(log_path_, 0), error);
            return !error;
        };
        bool was_stalled = false;
        bool snapshot_failure_logged = false;
        bool sampled_cap_logged = false;
        std::uint64_t sampled_lines = 0;
        std::optional<std::int64_t> last_incident_snapshot_ns;
        for (;;) {
            bool stalled = false;
            bool stopping;
            std::uint64_t dropped, untracked;
            batch.clear();
            {
                std::unique_lock lock(mutex_);
                cv_.wait(lock, [this] { return stopping_ || Enabled() || count_ > 0; });
                cv_.wait_for(lock, 250ms, [this] { return stopping_ || count_ >= kCapacity / 2; });
                for (std::size_t i = 0; i < count_; ++i) batch.push_back(queue_[(head_ + i) % kCapacity]);
                head_ = count_ = 0;
                dropped = dropped_.exchange(0);
                untracked = untracked_calls_.exchange(0);
                stopping = stopping_;
                if (Enabled() && !stopping) {
                    const auto ns = NowNs();
                    // Lost attempts are still producer activity, not evidence
                    // that the application's rendering thread stopped.
                    if (dropped || untracked) last_event_ns_ = ns;
                    for (auto& p : pending_) {
                        const auto token = p.token.load(std::memory_order_acquire);
                        if (token && ns - p.started_ns >= kStallNs) stalled = true;
                        if (token && ns - p.started_ns >= kStallNs &&
                            (!p.reported_ns || ns - p.reported_ns >= kReportNs)) {
                            batch.push_back({p.name, ns, p.thread, token, ++sequence_,
                                             ns - p.started_ns, 0, 0, true, "stall"});
                            p.reported_ns = ns;
                            burst_until_ = std::chrono::steady_clock::now() + 1s;
                        }
                    }
                    if (ns - last_event_ns_ >= kStallNs) stalled = true;
                    if (ns - last_event_ns_ >= kStallNs &&
                        (!idle_report_ns_ || ns - idle_report_ns_ >= kReportNs)) {
                        batch.push_back({"producer.idle", ns, ThreadId(), 0, ++sequence_,
                                         ns - last_event_ns_, 0, 0, true, nullptr});
                        idle_report_ns_ = ns;
                    }
                }
                if (dropped || untracked) batch.push_back({"records.dropped", NowNs(), ThreadId(), 0,
                    ++sequence_, static_cast<std::int64_t>(dropped), static_cast<std::int64_t>(untracked), 0,
                    true, nullptr});
            }
            const auto format_event = [&](const Event& e) {
                line = "Turbo-trace ns="; Number(line, e.ns);
                line += " thread="; Number(line, e.thread);
                line += " id="; Number(line, e.id);
                line += " event="; line += e.name;
                line += " a="; Number(line, e.a);
                line += " b="; Number(line, e.b);
                line += " c="; Number(line, e.c);
                line += " seq="; Number(line, e.sequence);
                line += " unixMs="; Number(line, wall_unix_ms_ + e.ns / 1'000'000);
                if (e.phase) { line += " phase="; line += e.phase; }
            };
            output.clear();
            for (const auto& e : batch) {
                format_event(e);
                // Bound a disk write to 64 KiB. No producer lock is held here.
                if (output.size() + line.size() + 1 > 64 * 1024) { persist(output); output.clear(); }
                output += line; output += '\n';
            }
            if (!output.empty()) persist(output);
            file.flush(); // OS-visible tail survives a process crash; not a power-loss guarantee.
            if (!file) throw std::runtime_error("Unable to flush flight recorder");
            // Incidents snapshot at most once per report interval: a fault that
            // repeats every frame must not turn into continuous 8 MiB copies.
            const auto now_ns = NowNs();
            const bool incident = std::any_of(batch.begin(), batch.end(),
                                              [](const Event& e) { return IsIncident(e.name); }) &&
                                  (!last_incident_snapshot_ns || now_ns - *last_incident_snapshot_ns >= kReportNs);
            if ((stalled && !was_stalled) || incident) {
                if (incident) last_incident_snapshot_ns = now_ns;
                if (!freeze() && !snapshot_failure_logged) {
                    snapshot_failure_logged = true;
                    logger.TryDebug("Turbo flight recorder: an incident snapshot could not be saved; "
                                    "continuous history is unaffected.");
                }
            }
            was_stalled = stalled;
            // Commit continuous history first. Sampled ordinary logging must
            // never make the recorder wait for a lock held by a game thread.
            for (const auto& e : batch) {
                if (!e.sampled) continue;
                if (sampled_lines >= kSampledLogLines) {
                    if (!sampled_cap_logged) {
                        sampled_cap_logged = logger.TryDebug(
                            "Turbo trace: the ordinary log reached its sampled-detail limit for this session; "
                            "the flight recorder files keep recording.");
                    }
                    break;
                }
                format_event(e);
                if (logger.TryDebug(line)) ++sampled_lines;
            }
            if (stopping) break;
        }
    } catch (...) {
        {
            std::scoped_lock lock(mutex_);
            writer_failed_ = true;
            enabled_.store(false, std::memory_order_release);
        }
        // Never let an allocation or filesystem failure terminate the VR app.
        try { logger.Error("Turbo flight recorder: background writer failed; diagnostics disabled for this session."); }
        catch (...) {}
    }
}
} // namespace depthxr
