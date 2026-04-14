#include "depthxr/seen_apps.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <exception>
#include <fstream>
#include <regex>
#include <sstream>
#include <system_error>
#include <vector>

#include "depthxr/config_path.h"

namespace depthxr {
namespace {

struct SeenAppObservation {
    std::string exe;
    std::int64_t first_seen_unix_seconds{0};
    std::int64_t last_seen_unix_seconds{0};
    std::int64_t launch_count{0};
};

std::string Basename(std::string value) {
    const size_t slash = value.find_last_of("/\\");
    if (slash != std::string::npos) {
        value = value.substr(slash + 1);
    }
    return value;
}

std::string Trim(std::string value) {
    const auto begin = std::find_if_not(value.begin(), value.end(), [](unsigned char c) {
        return std::isspace(c);
    });
    const auto end = std::find_if_not(value.rbegin(), value.rend(), [](unsigned char c) {
        return std::isspace(c);
    }).base();

    if (begin >= end) {
        return {};
    }
    return std::string(begin, end);
}

std::string NormalizeExe(std::string value) {
    value = Basename(Trim(std::move(value)));
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

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

std::vector<SeenAppObservation> ReadObservations(const std::filesystem::path& path) {
    std::ifstream stream(path);
    if (!stream) {
        return {};
    }

    std::ostringstream buffer;
    buffer << stream.rdbuf();
    const std::string content = buffer.str();

    std::vector<SeenAppObservation> observations;
    const std::regex object_pattern(R"json(\{[^{}]*"exe"\s*:\s*"((?:\\.|[^"\\])*)"[^{}]*\})json");
    for (auto it = std::sregex_iterator(content.begin(), content.end(), object_pattern);
         it != std::sregex_iterator();
         ++it) {
        const std::string object = it->str();
        SeenAppObservation observation;
        observation.exe = Basename(Trim(UnescapeJsonString((*it)[1].str())));
        if (observation.exe.empty()) {
            continue;
        }
        observation.first_seen_unix_seconds = ExtractInteger(object, "firstSeenUnixSeconds", 0);
        observation.last_seen_unix_seconds = ExtractInteger(object, "lastSeenUnixSeconds", 0);
        observation.launch_count = ExtractInteger(object, "launchCount", 0);
        observations.push_back(std::move(observation));
    }

    return observations;
}

std::string SerializeObservations(const std::vector<SeenAppObservation>& observations) {
    std::ostringstream stream;
    stream << "{\n";
    stream << "  \"version\": 1,\n";
    stream << "  \"observations\": [\n";
    for (size_t i = 0; i < observations.size(); ++i) {
        const SeenAppObservation& observation = observations[i];
        stream << "    {\n";
        stream << "      \"exe\": \"" << EscapeJsonString(observation.exe) << "\",\n";
        stream << "      \"firstSeenUnixSeconds\": " << observation.first_seen_unix_seconds << ",\n";
        stream << "      \"lastSeenUnixSeconds\": " << observation.last_seen_unix_seconds << ",\n";
        stream << "      \"launchCount\": " << observation.launch_count << "\n";
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
                *error = "Unable to create seen-apps directory: " + ec.message();
            }
            return false;
        }
    }

    const std::filesystem::path temporary_path = path.string() + ".tmp";
    {
        std::ofstream stream(temporary_path, std::ios::trunc);
        if (!stream) {
            if (error) {
                *error = "Unable to write seen-apps temp file: " + temporary_path.string();
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
            *error = "Unable to replace seen-apps file: " + ec.message();
        }
        return false;
    }
    return true;
}

std::int64_t CurrentUnixSeconds() {
    const auto now = std::chrono::system_clock::now();
    return std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();
}

} // namespace

bool RecordSeenAppObservation(const std::filesystem::path& path,
                              const std::string& exe_name,
                              std::int64_t unix_seconds,
                              std::string* error) {
    const std::string display_exe = Basename(Trim(exe_name));
    const std::string normalized_exe = NormalizeExe(display_exe);
    if (normalized_exe.empty()) {
        return true;
    }

    std::vector<SeenAppObservation> observations = ReadObservations(path);
    auto existing = std::find_if(observations.begin(), observations.end(), [&](const SeenAppObservation& observation) {
        return NormalizeExe(observation.exe) == normalized_exe;
    });

    if (existing == observations.end()) {
        observations.push_back({
            display_exe,
            unix_seconds,
            unix_seconds,
            1,
        });
    } else {
        if (existing->first_seen_unix_seconds <= 0) {
            existing->first_seen_unix_seconds = unix_seconds;
        }
        existing->last_seen_unix_seconds = unix_seconds;
        existing->launch_count = std::max<std::int64_t>(existing->launch_count, 0) + 1;
    }

    std::sort(observations.begin(), observations.end(), [](const SeenAppObservation& lhs, const SeenAppObservation& rhs) {
        return lhs.last_seen_unix_seconds > rhs.last_seen_unix_seconds;
    });

    return WriteFileAtomically(path, SerializeObservations(observations), error);
}

bool RecordSeenApp(const std::string& exe_name, std::string* error) {
    return RecordSeenAppObservation(ResolveSeenAppsPath(), exe_name, CurrentUnixSeconds(), error);
}

} // namespace depthxr
