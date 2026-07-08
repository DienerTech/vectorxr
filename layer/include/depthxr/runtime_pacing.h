#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "depthxr/settings.h"

namespace depthxr {

// One pacing verdict per OpenXR runtime, persisted to runtime-pacing.json
// (seen-apps pattern: layer-written facts, app-read, ships in debug reports).
// Settings carry user intent (pacing mode, pins); this file carries what Auto
// actually learned, so the two never race over settings.json.
struct RuntimePacingObservation {
    std::string runtime_name;
    std::string runtime_version;
    TurboPacingMode mode{TurboPacingMode::kAsync};
    // "preset" (seed table, never probed) or "discovered" (probed and
    // confirmed stable, or the fallback verdict after a probe failure).
    std::string source;
    std::string layer_version;
    std::int64_t first_used_unix_seconds{0};
    std::int64_t last_used_unix_seconds{0};
    // Discovery stats for support: async drain timeouts observed before the
    // verdict, and how long the winning mode ran clean.
    std::int64_t probe_timeouts{0};
    std::int64_t stable_seconds{0};
};

std::filesystem::path ResolveRuntimePacingPath();

std::vector<RuntimePacingObservation> ReadRuntimePacingObservations(const std::filesystem::path& path);

std::optional<RuntimePacingObservation> FindRuntimePacingObservation(const std::filesystem::path& path,
                                                                     const std::string& runtime_name);

// Upserts by runtime_name, preserving first_used and merging probe stats.
bool RecordRuntimePacingObservation(const std::filesystem::path& path,
                                    const RuntimePacingObservation& observation,
                                    std::string* error);

} // namespace depthxr
