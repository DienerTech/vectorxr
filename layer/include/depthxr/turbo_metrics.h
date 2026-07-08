#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace depthxr {

// Frame-pacing metrics for one pacing state ("off", "async", "sequenced")
// within a capture session. Derived values (averages, percentiles) are
// computed by the layer at flush time so the file carries display-ready
// facts, not raw histograms.
struct TurboMetricsBucket {
    std::string state;
    std::int64_t frames{0};
    // Captured rendering time in this state (sum of frame intervals).
    double seconds{0.0};
    double avg_fps{0.0};
    double avg_frame_ms{0.0};
    // 99th-percentile frame interval — the honest "pacing quality" number
    // (max is dominated by single hitches).
    double p99_frame_ms{0.0};
    double max_frame_ms{0.0};
    // Average per-frame time the app spent blocked on runtime pacing (its
    // own xrWaitFrame pass-through, the valve re-coupling wait, and turbo's
    // drain/sequenced-wait blocking inside EndFrame).
    double avg_wait_block_ms{0.0};
    std::int64_t fabricated_waits{0};
    std::int64_t drain_timeouts{0};
    // Frame intervals over 1s are excluded from the stats (loading stalls,
    // capture pauses) and counted here instead.
    std::int64_t discarded_frames{0};
};

// One capture session, persisted to turbo-metrics.json (seen-apps pattern:
// layer-written facts, app-read, ships in debug reports). Sessions are kept
// newest-first with a small retention cap so the schema already supports
// history without committing the UI to it.
struct TurboMetricsSession {
    std::string session_id;
    std::string app_name;
    std::string runtime_name;
    std::string layer_version;
    // "always" or "binding" — how capture was gated when recording.
    std::string collection_mode;
    // True while the session is still running (the layer flushes
    // periodically); the final flush at session teardown clears it.
    bool live{false};
    std::int64_t started_unix_seconds{0};
    std::int64_t updated_unix_seconds{0};
    std::vector<TurboMetricsBucket> buckets;
};

inline constexpr std::size_t kTurboMetricsRetainedSessions = 8;

std::filesystem::path ResolveTurboMetricsPath();

std::vector<TurboMetricsSession> ReadTurboMetricsSessions(const std::filesystem::path& path);

// Upserts by session_id, newest-first, truncating to the retention cap.
bool RecordTurboMetricsSession(const std::filesystem::path& path,
                               const TurboMetricsSession& session,
                               std::string* error);

} // namespace depthxr
