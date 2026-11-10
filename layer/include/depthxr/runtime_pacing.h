#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "depthxr/settings.h"

namespace depthxr {

// One pacing verdict per runtime/headset/graphics fingerprint, persisted to runtime-pacing.json
// (seen-apps pattern: layer-written facts, app-read, ships in debug reports).
// Settings carry user intent (pacing mode, pins); this file carries what Auto
// actually learned, so the two never race over settings.json.
struct RuntimePacingObservation {
    std::uint64_t test_started_at{0};
    std::string runtime_name;
    std::string runtime_version;
    std::string system_name;
    std::uint32_t vendor_id{0};
    std::string graphics_api;
    TurboPacingMode mode{TurboPacingMode::kAsync};
    // Only "discovered" records with a completed stability window are reused.
    std::string source;
    std::string layer_version;
    std::int64_t first_used_unix_seconds{0};
    std::int64_t last_used_unix_seconds{0};
    // Discovery stats for support: async drain timeouts observed before the
    // verdict, and how long the winning mode ran clean.
    std::int64_t probe_timeouts{0};
    std::int64_t stable_seconds{0};
    // Async probes in a row that fell back to sequenced within seconds of
    // engaging (TurboProbeGuard). One such verdict is provisional: the next
    // session probes async again. 0 in records from before 0.20.1.
    std::int64_t early_probe_failures{0};
};

// A recorded sequenced verdict is probed with async again once its evidence
// may be stale; the re-probe records its outcome either way, so each reason
// costs at most one probe (two when the re-probe also fails early):
//  - the async probe behind it fell back early, once (kEarlyProbeFailureLimit
//    early failures in a row and the verdict stands);
//  - it was recorded by another VectorXR version, or last used with another
//    runtime version (both empty-tolerant: no version, no reason);
//  - it is older than kReprobeAge (testStartedAt, the session that tested it).
// Async verdicts are never re-probed here: async failing later falls back in
// the session. Returns the reason, or nothing when the verdict stands.
inline constexpr std::int64_t kEarlyProbeFailureLimit = 2;
inline constexpr std::int64_t kReprobeAgeMs = 30LL * 24 * 60 * 60 * 1000;
std::optional<std::string> RuntimePacingReprobeReason(const RuntimePacingObservation& observation,
                                                      const std::string& layer_version,
                                                      const std::string& runtime_version,
                                                      std::int64_t now_unix_ms);

std::filesystem::path ResolveRuntimePacingPath();

std::vector<RuntimePacingObservation> ReadRuntimePacingObservations(const std::filesystem::path& path);

std::optional<RuntimePacingObservation> FindRuntimePacingObservation(const std::filesystem::path& path,
                                                                     const std::string& runtime_name,
                                                                     const std::string& system_name = {},
                                                                     std::uint32_t vendor_id = 0,
                                                                     const std::string& graphics_api = {});

// Upserts by runtime/system/graphics fingerprint, preserving first_used.
bool RecordRuntimePacingObservation(const std::filesystem::path& path,
                                    const RuntimePacingObservation& observation,
                                    std::string* error);

} // namespace depthxr
