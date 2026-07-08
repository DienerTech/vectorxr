#include "depthxr/turbo_metrics.h"

#include <algorithm>
#include <cstdio>
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

double ExtractDouble(const std::string& object, const std::string& key, double fallback) {
    const std::regex pattern("\"" + key + R"("\s*:\s*(-?\d+(?:\.\d+)?))");
    std::smatch match;
    if (!std::regex_search(object, match, pattern)) {
        return fallback;
    }

    try {
        return std::stod(match[1].str());
    } catch (const std::exception&) {
        return fallback;
    }
}

bool ExtractBool(const std::string& object, const std::string& key, bool fallback) {
    const std::regex pattern("\"" + key + R"("\s*:\s*(true|false))");
    std::smatch match;
    if (!std::regex_search(object, match, pattern)) {
        return fallback;
    }
    return match[1].str() == "true";
}

std::string ExtractString(const std::string& object, const std::string& key) {
    const std::regex pattern("\"" + key + R"json("\s*:\s*"((?:\\.|[^"\\])*)")json");
    std::smatch match;
    if (!std::regex_search(object, match, pattern)) {
        return {};
    }
    return UnescapeJsonString(match[1].str());
}

// Session objects nest one brace level (their buckets array), so they are
// found by brace-depth scanning rather than regex: MSVC's std::regex
// backtracks recursively and fail-fasts on nested-object patterns over more
// than a few kilobytes of input.
std::vector<std::string> ExtractSessionObjects(const std::string& content) {
    std::vector<std::string> objects;
    int depth = 0;
    bool in_string = false;
    bool escaped = false;
    size_t start = std::string::npos;
    for (size_t i = 0; i < content.size(); ++i) {
        const char c = content[i];
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (c == '\\') {
                escaped = true;
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }
        if (c == '"') {
            in_string = true;
        } else if (c == '{') {
            ++depth;
            // Depth 1 is the document root; depth 2 is a session object.
            if (depth == 2) {
                start = i;
            }
        } else if (c == '}') {
            if (depth == 2 && start != std::string::npos) {
                objects.push_back(content.substr(start, i - start + 1));
                start = std::string::npos;
            }
            --depth;
        }
    }
    return objects;
}

std::string FormatMetricDouble(double value) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.2f", value);
    return buffer;
}

std::string SerializeSessions(const std::vector<TurboMetricsSession>& sessions) {
    std::ostringstream stream;
    stream << "{\n";
    stream << "  \"version\": 1,\n";
    stream << "  \"sessions\": [\n";
    for (size_t i = 0; i < sessions.size(); ++i) {
        const TurboMetricsSession& session = sessions[i];
        stream << "    {\n";
        stream << "      \"sessionId\": \"" << EscapeJsonString(session.session_id) << "\",\n";
        stream << "      \"appName\": \"" << EscapeJsonString(session.app_name) << "\",\n";
        stream << "      \"runtimeName\": \"" << EscapeJsonString(session.runtime_name) << "\",\n";
        stream << "      \"layerVersion\": \"" << EscapeJsonString(session.layer_version) << "\",\n";
        stream << "      \"collectionMode\": \"" << EscapeJsonString(session.collection_mode) << "\",\n";
        stream << "      \"live\": " << (session.live ? "true" : "false") << ",\n";
        stream << "      \"startedUnixSeconds\": " << session.started_unix_seconds << ",\n";
        stream << "      \"updatedUnixSeconds\": " << session.updated_unix_seconds << ",\n";
        stream << "      \"buckets\": [\n";
        for (size_t j = 0; j < session.buckets.size(); ++j) {
            const TurboMetricsBucket& bucket = session.buckets[j];
            stream << "        {\n";
            stream << "          \"state\": \"" << EscapeJsonString(bucket.state) << "\",\n";
            stream << "          \"frames\": " << bucket.frames << ",\n";
            stream << "          \"seconds\": " << FormatMetricDouble(bucket.seconds) << ",\n";
            stream << "          \"avgFps\": " << FormatMetricDouble(bucket.avg_fps) << ",\n";
            stream << "          \"avgFrameMs\": " << FormatMetricDouble(bucket.avg_frame_ms) << ",\n";
            stream << "          \"p99FrameMs\": " << FormatMetricDouble(bucket.p99_frame_ms) << ",\n";
            stream << "          \"maxFrameMs\": " << FormatMetricDouble(bucket.max_frame_ms) << ",\n";
            stream << "          \"avgWaitBlockMs\": " << FormatMetricDouble(bucket.avg_wait_block_ms) << ",\n";
            stream << "          \"fabricatedWaits\": " << bucket.fabricated_waits << ",\n";
            stream << "          \"drainTimeouts\": " << bucket.drain_timeouts << ",\n";
            stream << "          \"discardedFrames\": " << bucket.discarded_frames << "\n";
            stream << "        }";
            if (j + 1 < session.buckets.size()) {
                stream << ",";
            }
            stream << "\n";
        }
        stream << "      ]\n";
        stream << "    }";
        if (i + 1 < sessions.size()) {
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
                *error = "Unable to create turbo-metrics directory: " + ec.message();
            }
            return false;
        }
    }

    const std::filesystem::path temporary_path = path.string() + ".tmp";
    {
        std::ofstream stream(temporary_path, std::ios::trunc);
        if (!stream) {
            if (error) {
                *error = "Unable to write turbo-metrics temp file: " + temporary_path.string();
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
            *error = "Unable to replace turbo-metrics file: " + ec.message();
        }
        return false;
    }
    return true;
}

} // namespace

std::filesystem::path ResolveTurboMetricsPath() {
    if (const char* env_path = std::getenv("VECTORXR_TURBO_METRICS_PATH"); env_path && *env_path != '\0') {
        return std::filesystem::path(env_path);
    }

#if defined(_WIN32)
    if (const char* local_app_data = std::getenv("LOCALAPPDATA"); local_app_data && *local_app_data != '\0') {
        return std::filesystem::path(local_app_data) / "VectorXR" / "config" / "turbo-metrics.json";
    }
#endif

    return std::filesystem::current_path() / "config" / "turbo-metrics.json";
}

std::vector<TurboMetricsSession> ReadTurboMetricsSessions(const std::filesystem::path& path) {
    std::ifstream stream(path);
    if (!stream) {
        return {};
    }

    std::ostringstream buffer;
    buffer << stream.rdbuf();
    const std::string content = buffer.str();

    std::vector<TurboMetricsSession> sessions;
    for (const std::string& object : ExtractSessionObjects(content)) {
        TurboMetricsSession session;
        session.session_id = ExtractString(object, "sessionId");
        if (session.session_id.empty()) {
            continue;
        }
        session.app_name = ExtractString(object, "appName");
        session.runtime_name = ExtractString(object, "runtimeName");
        session.layer_version = ExtractString(object, "layerVersion");
        session.collection_mode = ExtractString(object, "collectionMode");
        session.live = ExtractBool(object, "live", false);
        session.started_unix_seconds = ExtractInteger(object, "startedUnixSeconds", 0);
        session.updated_unix_seconds = ExtractInteger(object, "updatedUnixSeconds", 0);

        const size_t buckets_pos = object.find("\"buckets\"");
        if (buckets_pos != std::string::npos) {
            const std::string buckets_slice = object.substr(buckets_pos);
            const std::regex bucket_pattern(R"json(\{[^{}]*\})json");
            for (auto bucket_it = std::sregex_iterator(buckets_slice.begin(), buckets_slice.end(), bucket_pattern);
                 bucket_it != std::sregex_iterator();
                 ++bucket_it) {
                const std::string bucket_object = bucket_it->str();
                TurboMetricsBucket bucket;
                bucket.state = ExtractString(bucket_object, "state");
                if (bucket.state.empty()) {
                    continue;
                }
                bucket.frames = ExtractInteger(bucket_object, "frames", 0);
                bucket.seconds = ExtractDouble(bucket_object, "seconds", 0.0);
                bucket.avg_fps = ExtractDouble(bucket_object, "avgFps", 0.0);
                bucket.avg_frame_ms = ExtractDouble(bucket_object, "avgFrameMs", 0.0);
                bucket.p99_frame_ms = ExtractDouble(bucket_object, "p99FrameMs", 0.0);
                bucket.max_frame_ms = ExtractDouble(bucket_object, "maxFrameMs", 0.0);
                bucket.avg_wait_block_ms = ExtractDouble(bucket_object, "avgWaitBlockMs", 0.0);
                bucket.fabricated_waits = ExtractInteger(bucket_object, "fabricatedWaits", 0);
                bucket.drain_timeouts = ExtractInteger(bucket_object, "drainTimeouts", 0);
                bucket.discarded_frames = ExtractInteger(bucket_object, "discardedFrames", 0);
                session.buckets.push_back(std::move(bucket));
            }
        }

        sessions.push_back(std::move(session));
    }

    return sessions;
}

bool RecordTurboMetricsSession(const std::filesystem::path& path,
                               const TurboMetricsSession& session,
                               std::string* error) {
    if (session.session_id.empty()) {
        return true;
    }

    std::vector<TurboMetricsSession> sessions = ReadTurboMetricsSessions(path);
    auto existing = std::find_if(sessions.begin(), sessions.end(),
                                 [&](const TurboMetricsSession& candidate) {
                                     return candidate.session_id == session.session_id;
                                 });

    if (existing == sessions.end()) {
        sessions.push_back(session);
    } else {
        *existing = session;
    }

    std::sort(sessions.begin(), sessions.end(),
              [](const TurboMetricsSession& lhs, const TurboMetricsSession& rhs) {
                  return lhs.started_unix_seconds > rhs.started_unix_seconds;
              });
    if (sessions.size() > kTurboMetricsRetainedSessions) {
        sessions.resize(kTurboMetricsRetainedSessions);
    }

    return WriteFileAtomically(path, SerializeSessions(sessions), error);
}

} // namespace depthxr
