#include "depthxr/runtime_pacing.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <functional>
#include <regex>
#include <sstream>
#include <system_error>
#include <thread>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#endif

namespace depthxr {
namespace {

std::string EscapeJsonString(const std::string& value) {
    std::ostringstream stream;
    for (const char c : value) {
        switch (c) {
        case '\\':
            stream << "\\\\";
            break;
        case '"':
            stream << "\\\"";
            break;
        case '\b':
            stream << "\\b";
            break;
        case '\f':
            stream << "\\f";
            break;
        case '\n':
            stream << "\\n";
            break;
        case '\r':
            stream << "\\r";
            break;
        case '\t':
            stream << "\\t";
            break;
        default:
            stream << c;
            break;
        }
    }
    return stream.str();
}

std::string UnescapeJsonString(const std::string& value) {
    std::string result;
    for (size_t i = 0; i < value.size(); ++i) {
        if (value[i] != '\\' || i + 1 >= value.size()) {
            result.push_back(value[i]);
            continue;
        }

        const char escaped = value[++i];
        switch (escaped) {
        case '"':
        case '\\':
        case '/':
            result.push_back(escaped);
            break;
        case 'b':
            result.push_back('\b');
            break;
        case 'f':
            result.push_back('\f');
            break;
        case 'n':
            result.push_back('\n');
            break;
        case 'r':
            result.push_back('\r');
            break;
        case 't':
            result.push_back('\t');
            break;
        default:
            result.push_back(escaped);
            break;
        }
    }
    return result;
}

std::int64_t ExtractInteger(const std::string& object, const std::string& key, std::int64_t fallback) {
    const std::regex pattern("\"" + key + R"("\s*:\s*(-?\d+))");
    std::smatch match;
    if (!std::regex_search(object, match, pattern)) {
        return fallback;
    }

    try {
        return std::stoll(match[1].str());
    } catch (const std::exception&) {
        return fallback;
    }
}

std::string ExtractString(const std::string& object, const std::string& key) {
    const std::regex pattern("\"" + key + R"json("\s*:\s*"((?:\\.|[^"\\])*)")json");
    std::smatch match;
    if (!std::regex_search(object, match, pattern)) {
        return {};
    }
    return UnescapeJsonString(match[1].str());
}

std::string SerializeObservations(const std::vector<RuntimePacingObservation>& observations) {
    std::ostringstream stream;
    stream << "{\n";
    stream << "  \"version\": 2,\n";
    stream << "  \"observations\": [\n";
    for (size_t i = 0; i < observations.size(); ++i) {
        const RuntimePacingObservation& observation = observations[i];
        stream << "    {\n";
        stream << "      \"runtimeName\": \"" << EscapeJsonString(observation.runtime_name) << "\",\n";
        stream << "      \"runtimeVersion\": \"" << EscapeJsonString(observation.runtime_version) << "\",\n";
        stream << "      \"systemName\": \"" << EscapeJsonString(observation.system_name) << "\",\n";
        stream << "      \"vendorId\": " << observation.vendor_id << ",\n";
        stream << "      \"graphicsApi\": \"" << EscapeJsonString(observation.graphics_api) << "\",\n";
        stream << "      \"mode\": \"" << ToString(observation.mode) << "\",\n";
        stream << "      \"source\": \"" << EscapeJsonString(observation.source) << "\",\n";
        stream << "      \"layerVersion\": \"" << EscapeJsonString(observation.layer_version) << "\",\n";
        stream << "      \"firstUsedUnixSeconds\": " << observation.first_used_unix_seconds << ",\n";
        stream << "      \"lastUsedUnixSeconds\": " << observation.last_used_unix_seconds << ",\n";
        stream << "      \"probeTimeouts\": " << observation.probe_timeouts << ",\n";
        stream << "      \"stableSeconds\": " << observation.stable_seconds << "\n";
        stream << "    }";
        if (i + 1 < observations.size()) {
            stream << ",";
        }
        stream << "\n";
    }
    stream << "  ]\n";
    stream << "}\n";
    return stream.str();
}

bool WriteFileAtomically(const std::filesystem::path& path, const std::string& content, std::string* error) {
    std::error_code ec;
    if (const std::filesystem::path parent = path.parent_path(); !parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            if (error) {
                *error = "Unable to create runtime-pacing directory: " + ec.message();
            }
            return false;
        }
    }

    const auto unique_suffix = std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()) + "." +
        std::to_string(std::hash<std::thread::id>{}(std::this_thread::get_id()));
    const std::filesystem::path temporary_path = path.string() + ".tmp." + unique_suffix;
    {
        std::ofstream stream(temporary_path, std::ios::trunc);
        if (!stream) {
            if (error) {
                *error = "Unable to write runtime-pacing temp file: " + temporary_path.string();
            }
            return false;
        }
        stream << content;
    }

#if defined(_WIN32)
    // Antivirus/indexing can briefly open the just-written JSON without delete
    // sharing, making an otherwise valid atomic replacement fail with access
    // denied or a sharing violation. Retry for a short bounded interval; this
    // also keeps rapid test/app restarts from randomly losing a pacing verdict.
    DWORD move_error = ERROR_SUCCESS;
    constexpr int kMoveAttempts = 6;
    for (int attempt = 0; attempt < kMoveAttempts; ++attempt) {
        if (MoveFileExW(temporary_path.c_str(),
                        path.c_str(),
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            return true;
        }
        move_error = GetLastError();
        const bool transient = move_error == ERROR_ACCESS_DENIED || move_error == ERROR_SHARING_VIOLATION ||
                               move_error == ERROR_LOCK_VIOLATION;
        if (!transient || attempt + 1 == kMoveAttempts) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2u << attempt));
    }
    std::filesystem::remove(temporary_path, ec);
    if (error) {
        *error = "Unable to replace runtime-pacing file after retries: Win32 error " +
                 std::to_string(move_error);
    }
    return false;
#else
    std::filesystem::remove(path, ec);
    ec.clear();
    std::filesystem::rename(temporary_path, path, ec);
    if (ec) {
        std::filesystem::remove(temporary_path, ec);
        if (error) {
            *error = "Unable to replace runtime-pacing file: " + ec.message();
        }
        return false;
    }
#endif
    return true;
}

} // namespace

std::filesystem::path ResolveRuntimePacingPath() {
    if (const char* env_path = std::getenv("VECTORXR_RUNTIME_PACING_PATH"); env_path && *env_path != '\0') {
        return std::filesystem::path(env_path);
    }

#if defined(_WIN32)
    if (const char* local_app_data = std::getenv("LOCALAPPDATA"); local_app_data && *local_app_data != '\0') {
        return std::filesystem::path(local_app_data) / "VectorXR" / "config" / "runtime-pacing.json";
    }
#endif

    return std::filesystem::current_path() / "config" / "runtime-pacing.json";
}

std::vector<RuntimePacingObservation> ReadRuntimePacingObservations(const std::filesystem::path& path) {
    std::ifstream stream(path);
    if (!stream) {
        return {};
    }

    std::ostringstream buffer;
    buffer << stream.rdbuf();
    const std::string content = buffer.str();

    std::vector<RuntimePacingObservation> observations;
    const std::regex object_pattern(R"json(\{[^{}]*"runtimeName"\s*:\s*"(?:\\.|[^"\\])*"[^{}]*\})json");
    for (auto it = std::sregex_iterator(content.begin(), content.end(), object_pattern);
         it != std::sregex_iterator();
         ++it) {
        const std::string object = it->str();
        RuntimePacingObservation observation;
        observation.runtime_name = ExtractString(object, "runtimeName");
        if (observation.runtime_name.empty()) {
            continue;
        }
        observation.runtime_version = ExtractString(object, "runtimeVersion");
        observation.system_name = ExtractString(object, "systemName");
        observation.vendor_id = static_cast<std::uint32_t>(ExtractInteger(object, "vendorId", 0));
        observation.graphics_api = ExtractString(object, "graphicsApi");
        observation.mode = ParseTurboPacingMode(ExtractString(object, "mode")).value_or(TurboPacingMode::kAsync);
        observation.source = ExtractString(object, "source");
        observation.layer_version = ExtractString(object, "layerVersion");
        observation.first_used_unix_seconds = ExtractInteger(object, "firstUsedUnixSeconds", 0);
        observation.last_used_unix_seconds = ExtractInteger(object, "lastUsedUnixSeconds", 0);
        observation.probe_timeouts = ExtractInteger(object, "probeTimeouts", 0);
        observation.stable_seconds = ExtractInteger(object, "stableSeconds", 0);
        observations.push_back(std::move(observation));
    }

    return observations;
}

std::optional<RuntimePacingObservation> FindRuntimePacingObservation(const std::filesystem::path& path,
                                                                     const std::string& runtime_name,
                                                                     const std::string& system_name,
                                                                     std::uint32_t vendor_id,
                                                                     const std::string& graphics_api) {
    for (RuntimePacingObservation& observation : ReadRuntimePacingObservations(path)) {
        const bool runtime_matches = observation.runtime_name == runtime_name;
        const bool fingerprint_requested = !system_name.empty() || vendor_id != 0 || !graphics_api.empty();
        const bool fingerprint_matches = observation.system_name == system_name &&
                                         observation.vendor_id == vendor_id &&
                                         observation.graphics_api == graphics_api;
        if (runtime_matches && (!fingerprint_requested || fingerprint_matches)) {
            return std::move(observation);
        }
    }
    return std::nullopt;
}

bool RecordRuntimePacingObservation(const std::filesystem::path& path,
                                    const RuntimePacingObservation& observation,
                                    std::string* error) {
    if (observation.runtime_name.empty()) {
        return true;
    }

    std::vector<RuntimePacingObservation> observations = ReadRuntimePacingObservations(path);
    auto existing = std::find_if(observations.begin(), observations.end(),
                                 [&](const RuntimePacingObservation& candidate) {
                                     return candidate.runtime_name == observation.runtime_name &&
                                            candidate.system_name == observation.system_name &&
                                            candidate.vendor_id == observation.vendor_id &&
                                            candidate.graphics_api == observation.graphics_api;
                                 });

    if (existing == observations.end()) {
        observations.push_back(observation);
        if (observations.back().first_used_unix_seconds <= 0) {
            observations.back().first_used_unix_seconds = observations.back().last_used_unix_seconds;
        }
    } else {
        const std::int64_t first_used = existing->first_used_unix_seconds > 0
                                            ? existing->first_used_unix_seconds
                                            : observation.first_used_unix_seconds;
        *existing = observation;
        existing->first_used_unix_seconds =
            first_used > 0 ? first_used : observation.last_used_unix_seconds;
    }

    std::sort(observations.begin(), observations.end(),
              [](const RuntimePacingObservation& lhs, const RuntimePacingObservation& rhs) {
                  return lhs.last_used_unix_seconds > rhs.last_used_unix_seconds;
              });

    return WriteFileAtomically(path, SerializeObservations(observations), error);
}

} // namespace depthxr
