#include "depthxr/runtime_pacing.h"

#include <algorithm>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <regex>
#include <sstream>
#include <system_error>

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
    stream << "  \"version\": 1,\n";
    stream << "  \"observations\": [\n";
    for (size_t i = 0; i < observations.size(); ++i) {
        const RuntimePacingObservation& observation = observations[i];
        stream << "    {\n";
        stream << "      \"runtimeName\": \"" << EscapeJsonString(observation.runtime_name) << "\",\n";
        stream << "      \"runtimeVersion\": \"" << EscapeJsonString(observation.runtime_version) << "\",\n";
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

    const std::filesystem::path temporary_path = path.string() + ".tmp";
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
                                                                     const std::string& runtime_name) {
    for (RuntimePacingObservation& observation : ReadRuntimePacingObservations(path)) {
        if (observation.runtime_name == runtime_name) {
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
                                     return candidate.runtime_name == observation.runtime_name;
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
